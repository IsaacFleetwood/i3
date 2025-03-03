/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * commands_parser.c: hand-written parser to parse commands (commands are what
 * you bind on keys and what you can send to i3 using the IPC interface, like
 * 'move left' or 'workspace 4').
 *
 * We use a hand-written parser instead of lex/yacc because our commands are
 * easy for humans, not for computers. Thus, it’s quite hard to specify a
 * context-free grammar for the commands. A PEG grammar would be easier, but
 * there’s downsides to every PEG parser generator I have come across so far.
 *
 * This parser is basically a state machine which looks for literals or strings
 * and can push either on a stack. After identifying a literal or string, it
 * will either transition to the current state, to a different state, or call a
 * function (like cmd_move()).
 *
 * Special care has been taken that error messages are useful and the code is
 * well testable (when compiled with -DTEST_PARSER it will output to stdout
 * instead of actually calling any function).
 *
 */
#include "all.h"

// Macros to make the YAJL API a bit easier to use.
#define y(x, ...) (command_output.json_gen != NULL ? yajl_gen_##x(command_output.json_gen, ##__VA_ARGS__) : 0)
#define ystr(str) (command_output.json_gen != NULL ? yajl_gen_string(command_output.json_gen, (unsigned char *)str, strlen(str)) : 0)

/*******************************************************************************
 * The data structures used for parsing. Essentially the current state and a
 * list of tokens for that state.
 *
 * The GENERATED_* files are generated by generate-commands-parser.pl with the
 * input parser-specs/commands.spec.
 ******************************************************************************/

#include "GENERATED_command_enums.h"

typedef struct token {
    char *name;
    char *identifier;
    /* This might be __CALL */
    cmdp_state next_state;
    union {
        uint16_t call_identifier;
    } extra;
} cmdp_token;

typedef struct tokenptr {
    cmdp_token *array;
    int n;
} cmdp_token_ptr;

#include "GENERATED_command_tokens.h"

/*
 * Pushes a string (identified by 'identifier') on the stack. We simply use a
 * single array, since the number of entries we have to store is very small.
 *
 */
static void push_string(struct stack *stack, const char *identifier, char *str) {
    for (int c = 0; c < 10; c++) {
        if (stack->stack[c].identifier != NULL)
            continue;
        /* Found a free slot, let’s store it here. */
        stack->stack[c].identifier = identifier;
        stack->stack[c].val.str = str;
        stack->stack[c].type = STACK_STR;
        return;
    }

    /* When we arrive here, the stack is full. This should not happen and
     * means there’s either a bug in this parser or the specification
     * contains a command with more than 10 identified tokens. */
    fprintf(stderr, "BUG: commands_parser stack full. This means either a bug "
                    "in the code, or a new command which contains more than "
                    "10 identified tokens.\n");
    exit(EXIT_FAILURE);
}

// TODO move to a common util
static void push_long(struct stack *stack, const char *identifier, long num) {
    for (int c = 0; c < 10; c++) {
        if (stack->stack[c].identifier != NULL) {
            continue;
        }

        stack->stack[c].identifier = identifier;
        stack->stack[c].val.num = num;
        stack->stack[c].type = STACK_LONG;
        return;
    }

    /* When we arrive here, the stack is full. This should not happen and
     * means there’s either a bug in this parser or the specification
     * contains a command with more than 10 identified tokens. */
    fprintf(stderr, "BUG: commands_parser stack full. This means either a bug "
                    "in the code, or a new command which contains more than "
                    "10 identified tokens.\n");
    exit(EXIT_FAILURE);
}

// TODO move to a common util
static const char *get_string(struct stack *stack, const char *identifier) {
    for (int c = 0; c < 10; c++) {
        if (stack->stack[c].identifier == NULL)
            break;
        if (strcmp(identifier, stack->stack[c].identifier) == 0)
            return stack->stack[c].val.str;
    }
    return NULL;
}

// TODO move to a common util
static long get_long(struct stack *stack, const char *identifier) {
    for (int c = 0; c < 10; c++) {
        if (stack->stack[c].identifier == NULL)
            break;
        if (strcmp(identifier, stack->stack[c].identifier) == 0)
            return stack->stack[c].val.num;
    }

    return 0;
}

// TODO move to a common util
static void clear_stack(struct stack *stack) {
    for (int c = 0; c < 10; c++) {
        if (stack->stack[c].type == STACK_STR)
            free(stack->stack[c].val.str);
        stack->stack[c].identifier = NULL;
        stack->stack[c].val.str = NULL;
        stack->stack[c].val.num = 0;
    }
}

/*******************************************************************************
 * The parser itself.
 ******************************************************************************/

static cmdp_state state;
static Match current_match;
/*******************************************************************************
 * The (small) stack where identified literals are stored during the parsing
 * of a single command (like $workspace).
 ******************************************************************************/
static struct stack stack;
static struct CommandResultIR subcommand_output;
static struct CommandResultIR command_output;

#include "GENERATED_command_call.h"

static void next_state(const cmdp_token *token) {
    if (token->next_state == __CALL) {
        subcommand_output.json_gen = command_output.json_gen;
        subcommand_output.client = command_output.client;
        subcommand_output.needs_tree_render = false;
        GENERATED_call(&current_match, &stack, token->extra.call_identifier, &subcommand_output);
        state = subcommand_output.next_state;
        /* If any subcommand requires a tree_render(), we need to make the
         * whole parser result request a tree_render(). */
        if (subcommand_output.needs_tree_render)
            command_output.needs_tree_render = true;
        clear_stack(&stack);
        return;
    }

    state = token->next_state;
    if (state == INITIAL) {
        clear_stack(&stack);
    }
}

/*
 * Parses a string (or word, if as_word is true). Extracted out of
 * parse_command so that it can be used in src/workspace.c for interpreting
 * workspace commands.
 *
 */
char *parse_string(const char **walk, bool as_word) {
    const char *beginning = *walk;
    /* Handle quoted strings (or words). */
    if (**walk == '"') {
        beginning++;
        (*walk)++;
        for (; **walk != '\0' && **walk != '"'; (*walk)++)
            if (**walk == '\\' && *(*walk + 1) != '\0')
                (*walk)++;
    } else {
        if (!as_word) {
            /* For a string (starting with 's'), the delimiters are
             * comma (,) and semicolon (;) which introduce a new
             * operation or command, respectively. Also, newlines
             * end a command. */
            while (**walk != ';' && **walk != ',' &&
                   **walk != '\0' && **walk != '\r' &&
                   **walk != '\n')
                (*walk)++;
        } else {
            /* For a word, the delimiters are white space (' ' or
             * '\t'), closing square bracket (]), comma (,) and
             * semicolon (;). */
            while (**walk != ' ' && **walk != '\t' &&
                   **walk != ']' && **walk != ',' &&
                   **walk != ';' && **walk != '\r' &&
                   **walk != '\n' && **walk != '\0')
                (*walk)++;
        }
    }
    if (*walk == beginning)
        return NULL;

    char *str = scalloc(*walk - beginning + 1, 1);
    /* We copy manually to handle escaping of characters. */
    int inpos, outpos;
    for (inpos = 0, outpos = 0;
         inpos < (*walk - beginning);
         inpos++, outpos++) {
        /* We only handle escaped double quotes and backslashes to not break
         * backwards compatibility with people using \w in regular expressions
         * etc. */
        if (beginning[inpos] == '\\' && (beginning[inpos + 1] == '"' || beginning[inpos + 1] == '\\'))
            inpos++;
        str[outpos] = beginning[inpos];
    }

    return str;
}

/*
 * Parses and executes the given command. If a caller-allocated yajl_gen is
 * passed, a json reply will be generated in the format specified by the ipc
 * protocol. Pass NULL if no json reply is required.
 *
 * Free the returned CommandResult with command_result_free().
 */
CommandResult *parse_command(const char *input, yajl_gen gen, ipc_client *client) {
    DLOG("COMMAND: *%.4000s*\n", input);
    state = INITIAL;
    CommandResult *result = scalloc(1, sizeof(CommandResult));

    subcommand_output.execution_toggled = false;
    
    command_output.client = client;
    
    /* A YAJL JSON generator used for formatting replies. */
    command_output.json_gen = gen;

    y(array_open);
    command_output.needs_tree_render = false;

    const char *walk = input;
    const size_t len = strlen(input);
    int c;
    const cmdp_token *token;
    bool token_handled;

// TODO: make this testable
#ifndef TEST_PARSER
    cmd_criteria_init(&current_match, &subcommand_output);
#endif

    /* The "<=" operator is intentional: We also handle the terminating 0-byte
     * explicitly by looking for an 'end' token. */
    while ((size_t)(walk - input) <= len) {
        /* skip whitespace and newlines before every token */
        while ((*walk == ' ' || *walk == '\t' ||
                *walk == '\r' || *walk == '\n') &&
               *walk != '\0')
            walk++;
        
        cmdp_token_ptr *ptr = &(tokens[state]);
        token_handled = false;
        for (c = 0; c < ptr->n; c++) {
            token = &(ptr->array[c]);

            /* A literal. */
            if (token->name[0] == '\'') {
                if (strncasecmp(walk, token->name + 1, strlen(token->name) - 1) == 0) {
                    if (token->identifier != NULL) {
                        push_string(&stack, token->identifier, sstrdup(token->name + 1));
                    }
                    walk += strlen(token->name) - 1;
                    next_state(token);
                    token_handled = true;
                    break;
                }
                continue;
            }

            if (strcmp(token->name, "number") == 0) {
                /* Handle numbers. We only accept decimal numbers for now. */
                char *end = NULL;
                errno = 0;
                long int num = strtol(walk, &end, 10);
                if ((errno == ERANGE && (num == LONG_MIN || num == LONG_MAX)) ||
                    (errno != 0 && num == 0))
                    continue;

                /* No valid numbers found */
                if (end == walk)
                    continue;

                if (token->identifier != NULL) {
                    push_long(&stack, token->identifier, num);
                }

                /* Set walk to the first non-number character */
                walk = end;
                next_state(token);
                token_handled = true;
                break;
            }

            if (strcmp(token->name, "string") == 0 ||
                strcmp(token->name, "word") == 0) {
                char *str = parse_string(&walk, (token->name[0] != 's'));
                if (str != NULL) {
                    if (token->identifier) {
                        push_string(&stack, token->identifier, str);
                    }
                    /* If we are at the end of a quoted string, skip the ending
                     * double quote. */
                    if (*walk == '"')
                        walk++;
                    next_state(token);
                    token_handled = true;
                    break;
                }
            }

            if (strcmp(token->name, "end") == 0) {
                if (*walk == '\0' || *walk == ',' || *walk == ';') {
                    next_state(token);
                    token_handled = true;
                    /* To make sure we start with an appropriate matching
                     * datastructure for commands which do *not* specify any
                     * criteria, we re-initialize the criteria system after
                     * every command. */
// TODO: make this testable
#ifndef TEST_PARSER
                    if (*walk == '\0' || *walk == ';')
                        cmd_criteria_init(&current_match, &subcommand_output);
#endif
                    walk++;
                    break;
                }
            }
        }

        if (!token_handled) {
            /* Figure out how much memory we will need to fill in the names of
             * all tokens afterwards. */
            int tokenlen = 0;
            for (c = 0; c < ptr->n; c++)
                tokenlen += strlen(ptr->array[c].name) + strlen("'', ");

            /* Build up a decent error message. We include the problem, the
             * full input, and underline the position where the parser
             * currently is. */
            char *errormessage;
            char *possible_tokens = smalloc(tokenlen + 1);
            char *tokenwalk = possible_tokens;
            for (c = 0; c < ptr->n; c++) {
                token = &(ptr->array[c]);
                if (token->name[0] == '\'') {
                    /* A literal is copied to the error message enclosed with
                     * single quotes. */
                    *tokenwalk++ = '\'';
                    strcpy(tokenwalk, token->name + 1);
                    tokenwalk += strlen(token->name + 1);
                    *tokenwalk++ = '\'';
                } else {
                    /* Any other token is copied to the error message enclosed
                     * with angle brackets. */
                    *tokenwalk++ = '<';
                    strcpy(tokenwalk, token->name);
                    tokenwalk += strlen(token->name);
                    *tokenwalk++ = '>';
                }
                if (c < (ptr->n - 1)) {
                    *tokenwalk++ = ',';
                    *tokenwalk++ = ' ';
                }
            }
            *tokenwalk = '\0';
            sasprintf(&errormessage, "Expected one of these tokens: %s",
                      possible_tokens);
            free(possible_tokens);

            /* Contains the same amount of characters as 'input' has, but with
             * the unparsable part highlighted using ^ characters. */
            char *position = smalloc(len + 1);
            for (const char *copywalk = input; *copywalk != '\0'; copywalk++)
                position[(copywalk - input)] = (copywalk >= walk ? '^' : ' ');
            position[len] = '\0';

            ELOG("%s\n", errormessage);
            ELOG("Your command: %s\n", input);
            ELOG("              %s\n", position);

            result->parse_error = true;
            result->error_message = errormessage;

            /* Format this error message as a JSON reply. */
            y(map_open);
            ystr("success");
            y(bool, false);
            /* We set parse_error to true to distinguish this from other
             * errors. i3-nagbar is spawned upon keypresses only for parser
             * errors. */
            ystr("parse_error");
            y(bool, true);
            ystr("error");
            ystr(errormessage);
            ystr("input");
            ystr(input);
            ystr("errorposition");
            ystr(position);
            y(map_close);

            free(position);
            clear_stack(&stack);
            break;
        }
    }

    y(array_close);

    result->needs_tree_render = command_output.needs_tree_render;
    return result;
}

/*
 * Frees a CommandResult
 */
void command_result_free(CommandResult *result) {
    if (result == NULL)
        return;

    FREE(result->error_message);
    FREE(result);
}

/*******************************************************************************
 * Code for building the stand-alone binary test.commands_parser which is used
 * by t/187-commands-parser.t.
 ******************************************************************************/

#ifdef TEST_PARSER

/*
 * Logs the given message to stdout while prefixing the current time to it,
 * but only if debug logging was activated.
 * This is to be called by DLOG() which includes filename/linenumber
 *
 */
void debuglog(char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    fprintf(stdout, "# ");
    vfprintf(stdout, fmt, args);
    va_end(args);
}

void errorlog(char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Syntax: %s <command>\n", argv[0]);
        return 1;
    }
    yajl_gen gen = yajl_gen_alloc(NULL);

    CommandResult *result = parse_command(argv[1], gen, NULL);

    command_result_free(result);

    yajl_gen_free(gen);
}
#endif
