#undef I3__FILE__
#define I3__FILE__ "config_parser.c"
/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009-2013 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * config_parser.c: hand-written parser to parse configuration directives.
 *
 * See also src/commands_parser.c for rationale on why we use a custom parser.
 *
 * This parser works VERY MUCH like src/commands_parser.c, so read that first.
 * The differences are:
 *
 * 1. config_parser supports the 'number' token type (in addition to 'word' and
 *    'string'). Numbers are referred to using &num (like $str).
 *
 * 2. Criteria are not executed immediately, they are just stored.
 *
 * 3. config_parser recognizes \n and \r as 'end' token, while commands_parser
 *    ignores them.
 *
 * 4. config_parser skips the current line on invalid inputs and follows the
 *    nearest <error> token.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "all.h"

// Macros to make the YAJL API a bit easier to use.
#define y(x, ...) yajl_gen_ ## x (command_output.json_gen, ##__VA_ARGS__)
#define ystr(str) yajl_gen_string(command_output.json_gen, (unsigned char*)str, strlen(str))

#ifndef TEST_PARSER
pid_t config_error_nagbar_pid = -1;
static struct context *context;
#endif

/*******************************************************************************
 * The data structures used for parsing. Essentially the current state and a
 * list of tokens for that state.
 *
 * The GENERATED_* files are generated by generate-commands-parser.pl with the
 * input parser-specs/configs.spec.
 ******************************************************************************/

#include "GENERATED_config_enums.h"

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

#include "GENERATED_config_tokens.h"

/*******************************************************************************
 * The (small) stack where identified literals are stored during the parsing
 * of a single command (like $workspace).
 ******************************************************************************/

struct stack_entry {
    /* Just a pointer, not dynamically allocated. */
    const char *identifier;
    enum {
        STACK_STR = 0,
        STACK_LONG = 1,
    } type;
    union {
        char *str;
        long num;
    } val;
};

/* 10 entries should be enough for everybody. */
static struct stack_entry stack[10];

/*
 * Pushes a string (identified by 'identifier') on the stack. We simply use a
 * single array, since the number of entries we have to store is very small.
 *
 */
static void push_string(const char *identifier, const char *str) {
    for (int c = 0; c < 10; c++) {
        if (stack[c].identifier != NULL &&
            strcmp(stack[c].identifier, identifier) != 0)
            continue;
        if (stack[c].identifier == NULL) {
            /* Found a free slot, let’s store it here. */
            stack[c].identifier = identifier;
            stack[c].val.str = sstrdup(str);
            stack[c].type = STACK_STR;
        } else {
            /* Append the value. */
            char *prev = stack[c].val.str;
            sasprintf(&(stack[c].val.str), "%s,%s", prev, str);
            free(prev);
        }
        return;
    }

    /* When we arrive here, the stack is full. This should not happen and
     * means there’s either a bug in this parser or the specification
     * contains a command with more than 10 identified tokens. */
    fprintf(stderr, "BUG: commands_parser stack full. This means either a bug "
                    "in the code, or a new command which contains more than "
                    "10 identified tokens.\n");
    exit(1);
}

static void push_long(const char *identifier, long num) {
    for (int c = 0; c < 10; c++) {
        if (stack[c].identifier != NULL)
            continue;
        /* Found a free slot, let’s store it here. */
        stack[c].identifier = identifier;
        stack[c].val.num = num;
        stack[c].type = STACK_LONG;
        return;
    }

    /* When we arrive here, the stack is full. This should not happen and
     * means there’s either a bug in this parser or the specification
     * contains a command with more than 10 identified tokens. */
    fprintf(stderr, "BUG: commands_parser stack full. This means either a bug "
                    "in the code, or a new command which contains more than "
                    "10 identified tokens.\n");
    exit(1);

}

static const char *get_string(const char *identifier) {
    for (int c = 0; c < 10; c++) {
        if (stack[c].identifier == NULL)
            break;
        if (strcmp(identifier, stack[c].identifier) == 0)
            return stack[c].val.str;
    }
    return NULL;
}

static const long get_long(const char *identifier) {
    for (int c = 0; c < 10; c++) {
        if (stack[c].identifier == NULL)
            break;
        if (strcmp(identifier, stack[c].identifier) == 0)
            return stack[c].val.num;
    }
    return 0;
}

static void clear_stack(void) {
    for (int c = 0; c < 10; c++) {
        if (stack[c].type == STACK_STR && stack[c].val.str != NULL)
            free(stack[c].val.str);
        stack[c].identifier = NULL;
        stack[c].val.str = NULL;
        stack[c].val.num = 0;
    }
}

// TODO: remove this if it turns out we don’t need it for testing.
#if 0
/*******************************************************************************
 * A dynamically growing linked list which holds the criteria for the current
 * command.
 ******************************************************************************/

typedef struct criterion {
    char *type;
    char *value;

    TAILQ_ENTRY(criterion) criteria;
} criterion;

static TAILQ_HEAD(criteria_head, criterion) criteria =
  TAILQ_HEAD_INITIALIZER(criteria);

/*
 * Stores the given type/value in the list of criteria.
 * Accepts a pointer as first argument, since it is 'call'ed by the parser.
 *
 */
static void push_criterion(void *unused_criteria, const char *type,
                           const char *value) {
    struct criterion *criterion = malloc(sizeof(struct criterion));
    criterion->type = strdup(type);
    criterion->value = strdup(value);
    TAILQ_INSERT_TAIL(&criteria, criterion, criteria);
}

/*
 * Clears the criteria linked list.
 * Accepts a pointer as first argument, since it is 'call'ed by the parser.
 *
 */
static void clear_criteria(void *unused_criteria) {
    struct criterion *criterion;
    while (!TAILQ_EMPTY(&criteria)) {
        criterion = TAILQ_FIRST(&criteria);
        free(criterion->type);
        free(criterion->value);
        TAILQ_REMOVE(&criteria, criterion, criteria);
        free(criterion);
    }
}
#endif

/*******************************************************************************
 * The parser itself.
 ******************************************************************************/

static cmdp_state state;
static Match current_match;
static struct ConfigResult subcommand_output;
static struct ConfigResult command_output;

/* A list which contains the states that lead to the current state, e.g.
 * INITIAL, WORKSPACE_LAYOUT.
 * When jumping back to INITIAL, statelist_idx will simply be set to 1
 * (likewise for other states, e.g. MODE or BAR).
 * This list is used to process the nearest error token. */
static cmdp_state statelist[10] = { INITIAL };
/* NB: statelist_idx points to where the next entry will be inserted */
static int statelist_idx = 1;

#include "GENERATED_config_call.h"


static void next_state(const cmdp_token *token) {
    cmdp_state _next_state = token->next_state;

	//printf("token = name %s identifier %s\n", token->name, token->identifier);
	//printf("next_state = %d\n", token->next_state);
    if (token->next_state == __CALL) {
        subcommand_output.json_gen = command_output.json_gen;
        GENERATED_call(token->extra.call_identifier, &subcommand_output);
        _next_state = subcommand_output.next_state;
        clear_stack();
    }

    state = _next_state;
    if (state == INITIAL) {
        clear_stack();
    }

    /* See if we are jumping back to a state in which we were in previously
     * (statelist contains INITIAL) and just move statelist_idx accordingly. */
    for (int i = 0; i < statelist_idx; i++) {
        if (statelist[i] != _next_state)
            continue;
        statelist_idx = i+1;
        return;
    }

    /* Otherwise, the state is new and we add it to the list */
    statelist[statelist_idx++] = _next_state;
}

/*
 * Returns a pointer to the start of the line (one byte after the previous \r,
 * \n) or the start of the input, if this is the first line.
 *
 */
static const char *start_of_line(const char *walk, const char *beginning) {
    while (*walk != '\n' && *walk != '\r' && walk >= beginning) {
        walk--;
    }

    return walk + 1;
}

/*
 * Copies the line and terminates it at the next \n, if any.
 *
 * The caller has to free() the result.
 *
 */
static char *single_line(const char *start) {
    char *result = sstrdup(start);
    char *end = strchr(result, '\n');
    if (end != NULL)
        *end = '\0';
    return result;
}

struct ConfigResult *parse_config(const char *input, struct context *context) {
    /* Dump the entire config file into the debug log. We cannot just use
     * DLOG("%s", input); because one log message must not exceed 4 KiB. */
    const char *dumpwalk = input;
    int linecnt = 1;
    while (*dumpwalk != '\0') {
        char *next_nl = strchr(dumpwalk, '\n');
        if (next_nl != NULL) {
            DLOG("CONFIG(line %3d): %.*s\n", linecnt, (int)(next_nl - dumpwalk), dumpwalk);
            dumpwalk = next_nl + 1;
        } else {
            DLOG("CONFIG(line %3d): %s\n", linecnt, dumpwalk);
            break;
        }
        linecnt++;
    }
    state = INITIAL;
    statelist_idx = 1;

/* A YAJL JSON generator used for formatting replies. */
#if YAJL_MAJOR >= 2
    command_output.json_gen = yajl_gen_alloc(NULL);
#else
    command_output.json_gen = yajl_gen_alloc(NULL, NULL);
#endif

    y(array_open);

    const char *walk = input;
    const size_t len = strlen(input);
    int c;
    const cmdp_token *token;
    bool token_handled;
    linecnt = 1;

    // TODO: make this testable
#ifndef TEST_PARSER
    cfg_criteria_init(&current_match, &subcommand_output, INITIAL);
#endif

    /* The "<=" operator is intentional: We also handle the terminating 0-byte
     * explicitly by looking for an 'end' token. */
    while ((walk - input) <= len) {
        /* Skip whitespace before every token, newlines are relevant since they
         * separate configuration directives. */
        while ((*walk == ' ' || *walk == '\t') && *walk != '\0')
            walk++;

		//printf("remaining input: %s\n", walk);

        cmdp_token_ptr *ptr = &(tokens[state]);
        token_handled = false;
        for (c = 0; c < ptr->n; c++) {
            token = &(ptr->array[c]);

            /* A literal. */
            if (token->name[0] == '\'') {
                if (strncasecmp(walk, token->name + 1, strlen(token->name) - 1) == 0) {
                    if (token->identifier != NULL)
                        push_string(token->identifier, token->name + 1);
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

                if (token->identifier != NULL)
                    push_long(token->identifier, num);

                /* Set walk to the first non-number character */
                walk = end;
                next_state(token);
                token_handled = true;
                break;
            }

            if (strcmp(token->name, "string") == 0 ||
                strcmp(token->name, "word") == 0) {
                const char *beginning = walk;
                /* Handle quoted strings (or words). */
                if (*walk == '"') {
                    beginning++;
                    walk++;
                    while (*walk != '\0' && (*walk != '"' || *(walk-1) == '\\'))
                        walk++;
                } else {
                    if (token->name[0] == 's') {
                        while (*walk != '\0' && *walk != '\r' && *walk != '\n')
                            walk++;
                    } else {
                        /* For a word, the delimiters are white space (' ' or
                         * '\t'), closing square bracket (]), comma (,) and
                         * semicolon (;). */
                        while (*walk != ' ' && *walk != '\t' &&
                               *walk != ']' && *walk != ',' &&
                               *walk !=  ';' && *walk != '\r' &&
                               *walk != '\n' && *walk != '\0')
                            walk++;
                    }
                }
                if (walk != beginning) {
                    char *str = scalloc(walk-beginning + 1);
                    /* We copy manually to handle escaping of characters. */
                    int inpos, outpos;
                    for (inpos = 0, outpos = 0;
                         inpos < (walk-beginning);
                         inpos++, outpos++) {
                        /* We only handle escaped double quotes to not break
                         * backwards compatibility with people using \w in
                         * regular expressions etc. */
                        if (beginning[inpos] == '\\' && beginning[inpos+1] == '"')
                            inpos++;
                        str[outpos] = beginning[inpos];
                    }
                    if (token->identifier)
                        push_string(token->identifier, str);
                    free(str);
                    /* If we are at the end of a quoted string, skip the ending
                     * double quote. */
                    if (*walk == '"')
                        walk++;
                    next_state(token);
                    token_handled = true;
                    break;
                }
            }

            if (strcmp(token->name, "line") == 0) {
               while (*walk != '\0' && *walk != '\n' && *walk != '\r')
                  walk++;
               next_state(token);
               token_handled = true;
               linecnt++;
               walk++;
               break;
            }

            if (strcmp(token->name, "end") == 0) {
                //printf("checking for end: *%s*\n", walk);
                if (*walk == '\0' || *walk == '\n' || *walk == '\r') {
                    next_state(token);
                    token_handled = true;
                    /* To make sure we start with an appropriate matching
                     * datastructure for commands which do *not* specify any
                     * criteria, we re-initialize the criteria system after
                     * every command. */
                    // TODO: make this testable
#ifndef TEST_PARSER
                    cfg_criteria_init(&current_match, &subcommand_output, INITIAL);
#endif
                    linecnt++;
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
                    /* Skip error tokens in error messages, they are used
                     * internally only and might confuse users. */
                    if (strcmp(token->name, "error") == 0)
                        continue;
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


            /* Go back to the beginning of the line */
            const char *error_line = start_of_line(walk, input);

            /* Contains the same amount of characters as 'input' has, but with
             * the unparseable part highlighted using ^ characters. */
            char *position = scalloc(strlen(error_line) + 1);
            const char *copywalk;
            for (copywalk = error_line;
                 *copywalk != '\n' && *copywalk != '\r' && *copywalk != '\0';
                 copywalk++)
                position[(copywalk - error_line)] = (copywalk >= walk ? '^' : (*copywalk == '\t' ? '\t' : ' '));
            position[(copywalk - error_line)] = '\0';

            ELOG("CONFIG: %s\n", errormessage);
            ELOG("CONFIG: (in file %s)\n", context->filename);
            char *error_copy = single_line(error_line);

            /* Print context lines *before* the error, if any. */
            if (linecnt > 1) {
                const char *context_p1_start = start_of_line(error_line-2, input);
                char *context_p1_line = single_line(context_p1_start);
                if (linecnt > 2) {
                    const char *context_p2_start = start_of_line(context_p1_start-2, input);
                    char *context_p2_line = single_line(context_p2_start);
                    ELOG("CONFIG: Line %3d: %s\n", linecnt - 2, context_p2_line);
                    free(context_p2_line);
                }
                ELOG("CONFIG: Line %3d: %s\n", linecnt - 1, context_p1_line);
                free(context_p1_line);
            }
            ELOG("CONFIG: Line %3d: %s\n", linecnt, error_copy);
            ELOG("CONFIG:           %s\n", position);
            free(error_copy);
            /* Print context lines *after* the error, if any. */
            for (int i = 0; i < 2; i++) {
                char *error_line_end = strchr(error_line, '\n');
                if (error_line_end != NULL && *(error_line_end + 1) != '\0') {
                    error_line = error_line_end + 1;
                    error_copy = single_line(error_line);
                    ELOG("CONFIG: Line %3d: %s\n", linecnt + i + 1, error_copy);
                    free(error_copy);
                }
            }

            context->has_errors = true;

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

            /* Skip the rest of this line, but continue parsing. */
            while ((walk - input) <= len && *walk != '\n')
                walk++;

            free(position);
            free(errormessage);
            clear_stack();

            /* To figure out in which state to go (e.g. MODE or INITIAL),
             * we find the nearest state which contains an <error> token
             * and follow that one. */
            bool error_token_found = false;
            for (int i = statelist_idx-1; (i >= 0) && !error_token_found; i--) {
                cmdp_token_ptr *errptr = &(tokens[statelist[i]]);
                for (int j = 0; j < errptr->n; j++) {
                    if (strcmp(errptr->array[j].name, "error") != 0)
                        continue;
                    next_state(&(errptr->array[j]));
                    error_token_found = true;
                    break;
                }
            }

            assert(error_token_found);
        }
    }

    y(array_close);

    return &command_output;
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

static int criteria_next_state;

void cfg_criteria_init(I3_CFG, int _state) {
    criteria_next_state = _state;
}

void cfg_criteria_add(I3_CFG, const char *ctype, const char *cvalue) {
}

void cfg_criteria_pop_state(I3_CFG) {
    result->next_state = criteria_next_state;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Syntax: %s <command>\n", argv[0]);
        return 1;
    }
    struct context context;
    context.filename = "<stdin>";
    parse_config(argv[1], &context);
}

#else

/*
 * Goes through each line of buf (separated by \n) and checks for statements /
 * commands which only occur in i3 v4 configuration files. If it finds any, it
 * returns version 4, otherwise it returns version 3.
 *
 */
static int detect_version(char *buf) {
    char *walk = buf;
    char *line = buf;
    while (*walk != '\0') {
        if (*walk != '\n') {
            walk++;
            continue;
        }

        /* check for some v4-only statements */
        if (strncasecmp(line, "bindcode", strlen("bindcode")) == 0 ||
            strncasecmp(line, "force_focus_wrapping", strlen("force_focus_wrapping")) == 0 ||
            strncasecmp(line, "# i3 config file (v4)", strlen("# i3 config file (v4)")) == 0 ||
            strncasecmp(line, "workspace_layout", strlen("workspace_layout")) == 0) {
            printf("deciding for version 4 due to this line: %.*s\n", (int)(walk-line), line);
            return 4;
        }

        /* if this is a bind statement, we can check the command */
        if (strncasecmp(line, "bind", strlen("bind")) == 0) {
            char *bind = strchr(line, ' ');
            if (bind == NULL)
                goto next;
            while ((*bind == ' ' || *bind == '\t') && *bind != '\0')
                bind++;
            if (*bind == '\0')
                goto next;
            if ((bind = strchr(bind, ' ')) == NULL)
                goto next;
            while ((*bind == ' ' || *bind == '\t') && *bind != '\0')
                bind++;
            if (*bind == '\0')
                goto next;
            if (strncasecmp(bind, "layout", strlen("layout")) == 0 ||
                strncasecmp(bind, "floating", strlen("floating")) == 0 ||
                strncasecmp(bind, "workspace", strlen("workspace")) == 0 ||
                strncasecmp(bind, "focus left", strlen("focus left")) == 0 ||
                strncasecmp(bind, "focus right", strlen("focus right")) == 0 ||
                strncasecmp(bind, "focus up", strlen("focus up")) == 0 ||
                strncasecmp(bind, "focus down", strlen("focus down")) == 0 ||
                strncasecmp(bind, "border normal", strlen("border normal")) == 0 ||
                strncasecmp(bind, "border 1pixel", strlen("border 1pixel")) == 0 ||
                strncasecmp(bind, "border pixel", strlen("border pixel")) == 0 ||
                strncasecmp(bind, "border borderless", strlen("border borderless")) == 0 ||
                strncasecmp(bind, "--no-startup-id", strlen("--no-startup-id")) == 0 ||
                strncasecmp(bind, "bar", strlen("bar")) == 0) {
                printf("deciding for version 4 due to this line: %.*s\n", (int)(walk-line), line);
                return 4;
            }
        }

next:
        /* advance to the next line */
        walk++;
        line = walk;
    }

    return 3;
}

/*
 * Calls i3-migrate-config-to-v4 to migrate a configuration file (input
 * buffer).
 *
 * Returns the converted config file or NULL if there was an error (for
 * example the script could not be found in $PATH or the i3 executable’s
 * directory).
 *
 */
static char *migrate_config(char *input, off_t size) {
    int writepipe[2];
    int readpipe[2];

    if (pipe(writepipe) != 0 ||
        pipe(readpipe) != 0) {
        warn("migrate_config: Could not create pipes");
        return NULL;
    }

    pid_t pid = fork();
    if (pid == -1) {
        warn("Could not fork()");
        return NULL;
    }

    /* child */
    if (pid == 0) {
        /* close writing end of writepipe, connect reading side to stdin */
        close(writepipe[1]);
        dup2(writepipe[0], 0);

        /* close reading end of readpipe, connect writing side to stdout */
        close(readpipe[0]);
        dup2(readpipe[1], 1);

        static char *argv[] = {
            NULL, /* will be replaced by the executable path */
            NULL
        };
        exec_i3_utility("i3-migrate-config-to-v4", argv);
    }

    /* parent */

    /* close reading end of the writepipe (connected to the script’s stdin) */
    close(writepipe[0]);

    /* write the whole config file to the pipe, the script will read everything
     * immediately */
    int written = 0;
    int ret;
    while (written < size) {
        if ((ret = write(writepipe[1], input + written, size - written)) < 0) {
            warn("Could not write to pipe");
            return NULL;
        }
        written += ret;
    }
    close(writepipe[1]);

    /* close writing end of the readpipe (connected to the script’s stdout) */
    close(readpipe[1]);

    /* read the script’s output */
    int conv_size = 65535;
    char *converted = malloc(conv_size);
    int read_bytes = 0;
    do {
        if (read_bytes == conv_size) {
            conv_size += 65535;
            converted = realloc(converted, conv_size);
        }
        ret = read(readpipe[0], converted + read_bytes, conv_size - read_bytes);
        if (ret == -1) {
            warn("Cannot read from pipe");
            FREE(converted);
            return NULL;
        }
        read_bytes += ret;
    } while (ret > 0);

    /* get the returncode */
    int status;
    wait(&status);
    if (!WIFEXITED(status)) {
        fprintf(stderr, "Child did not terminate normally, using old config file (will lead to broken behaviour)\n");
        return NULL;
    }

    int returncode = WEXITSTATUS(status);
    if (returncode != 0) {
        fprintf(stderr, "Migration process exit code was != 0\n");
        if (returncode == 2) {
            fprintf(stderr, "could not start the migration script\n");
            /* TODO: script was not found. tell the user to fix his system or create a v4 config */
        } else if (returncode == 1) {
            fprintf(stderr, "This already was a v4 config. Please add the following line to your config file:\n");
            fprintf(stderr, "# i3 config file (v4)\n");
            /* TODO: nag the user with a message to include a hint for i3 in his config file */
        }
        return NULL;
    }

    return converted;
}

/*
 * Checks for duplicate key bindings (the same keycode or keysym is configured
 * more than once). If a duplicate binding is found, a message is printed to
 * stderr and the has_errors variable is set to true, which will start
 * i3-nagbar.
 *
 */
static void check_for_duplicate_bindings(struct context *context) {
    Binding *bind, *current;
    TAILQ_FOREACH(current, bindings, bindings) {
        TAILQ_FOREACH(bind, bindings, bindings) {
            /* Abort when we reach the current keybinding, only check the
             * bindings before */
            if (bind == current)
                break;

            /* Check if one is using keysym while the other is using bindsym.
             * If so, skip. */
            /* XXX: It should be checked at a later place (when translating the
             * keysym to keycodes) if there are any duplicates */
            if ((bind->symbol == NULL && current->symbol != NULL) ||
                (bind->symbol != NULL && current->symbol == NULL))
                continue;

            /* If bind is NULL, current has to be NULL, too (see above).
             * If the keycodes differ, it can't be a duplicate. */
            if (bind->symbol != NULL &&
                strcasecmp(bind->symbol, current->symbol) != 0)
                continue;

            /* Check if the keycodes or modifiers are different. If so, they
             * can't be duplicate */
            if (bind->keycode != current->keycode ||
                bind->mods != current->mods ||
                bind->release != current->release)
                continue;

            context->has_errors = true;
            if (current->keycode != 0) {
                ELOG("Duplicate keybinding in config file:\n  modmask %d with keycode %d, command \"%s\"\n",
                     current->mods, current->keycode, current->command);
            } else {
                ELOG("Duplicate keybinding in config file:\n  modmask %d with keysym %s, command \"%s\"\n",
                     current->mods, current->symbol, current->command);
            }
        }
    }
}

/*
 * Parses the given file by first replacing the variables, then calling
 * parse_config and possibly launching i3-nagbar.
 *
 */
void parse_file(const char *f) {
    SLIST_HEAD(variables_head, Variable) variables = SLIST_HEAD_INITIALIZER(&variables);
    int fd, ret, read_bytes = 0;
    struct stat stbuf;
    char *buf;
    FILE *fstr;
    char buffer[1026], key[512], value[512];

    if ((fd = open(f, O_RDONLY)) == -1)
        die("Could not open configuration file: %s\n", strerror(errno));

    if (fstat(fd, &stbuf) == -1)
        die("Could not fstat file: %s\n", strerror(errno));

    buf = scalloc((stbuf.st_size + 1) * sizeof(char));
    while (read_bytes < stbuf.st_size) {
        if ((ret = read(fd, buf + read_bytes, (stbuf.st_size - read_bytes))) < 0)
            die("Could not read(): %s\n", strerror(errno));
        read_bytes += ret;
    }

    if (lseek(fd, 0, SEEK_SET) == (off_t)-1)
        die("Could not lseek: %s\n", strerror(errno));

    if ((fstr = fdopen(fd, "r")) == NULL)
        die("Could not fdopen: %s\n", strerror(errno));

    while (!feof(fstr)) {
        if (fgets(buffer, 1024, fstr) == NULL) {
            if (feof(fstr))
                break;
            die("Could not read configuration file\n");
        }

        /* sscanf implicitly strips whitespace. Also, we skip comments and empty lines. */
        if (sscanf(buffer, "%s %[^\n]", key, value) < 1 ||
            key[0] == '#' || strlen(key) < 3)
            continue;

        if (strcasecmp(key, "set") == 0) {
            if (value[0] != '$') {
                ELOG("Malformed variable assignment, name has to start with $\n");
                continue;
            }

            /* get key/value for this variable */
            char *v_key = value, *v_value;
            if (strstr(value, " ") == NULL && strstr(value, "\t") == NULL) {
                ELOG("Malformed variable assignment, need a value\n");
                continue;
            }

            if (!(v_value = strstr(value, " ")))
                v_value = strstr(value, "\t");

            *(v_value++) = '\0';
            while (*v_value == '\t' || *v_value == ' ')
                v_value++;

            struct Variable *new = scalloc(sizeof(struct Variable));
            new->key = sstrdup(v_key);
            new->value = sstrdup(v_value);
            SLIST_INSERT_HEAD(&variables, new, variables);
            DLOG("Got new variable %s = %s\n", v_key, v_value);
            continue;
        }
    }
    fclose(fstr);

    /* For every custom variable, see how often it occurs in the file and
     * how much extra bytes it requires when replaced. */
    struct Variable *current, *nearest;
    int extra_bytes = 0;
    /* We need to copy the buffer because we need to invalidate the
     * variables (otherwise we will count them twice, which is bad when
     * 'extra' is negative) */
    char *bufcopy = sstrdup(buf);
    SLIST_FOREACH(current, &variables, variables) {
        int extra = (strlen(current->value) - strlen(current->key));
        char *next;
        for (next = bufcopy;
             next < (bufcopy + stbuf.st_size) &&
             (next = strcasestr(next, current->key)) != NULL;
             next += strlen(current->key)) {
            *next = '_';
            extra_bytes += extra;
        }
    }
    FREE(bufcopy);

    /* Then, allocate a new buffer and copy the file over to the new one,
     * but replace occurences of our variables */
    char *walk = buf, *destwalk;
    char *new = smalloc((stbuf.st_size + extra_bytes + 1) * sizeof(char));
    destwalk = new;
    while (walk < (buf + stbuf.st_size)) {
        /* Find the next variable */
        SLIST_FOREACH(current, &variables, variables)
            current->next_match = strcasestr(walk, current->key);
        nearest = NULL;
        int distance = stbuf.st_size;
        SLIST_FOREACH(current, &variables, variables) {
            if (current->next_match == NULL)
                continue;
            if ((current->next_match - walk) < distance) {
                distance = (current->next_match - walk);
                nearest = current;
            }
        }
        if (nearest == NULL) {
            /* If there are no more variables, we just copy the rest */
            strncpy(destwalk, walk, (buf + stbuf.st_size) - walk);
            destwalk += (buf + stbuf.st_size) - walk;
            *destwalk = '\0';
            break;
        } else {
            /* Copy until the next variable, then copy its value */
            strncpy(destwalk, walk, distance);
            strncpy(destwalk + distance, nearest->value, strlen(nearest->value));
            walk += distance + strlen(nearest->key);
            destwalk += distance + strlen(nearest->value);
        }
    }

    /* analyze the string to find out whether this is an old config file (3.x)
     * or a new config file (4.x). If it’s old, we run the converter script. */
    int version = detect_version(buf);
    if (version == 3) {
        /* We need to convert this v3 configuration */
        char *converted = migrate_config(new, stbuf.st_size);
        if (converted != NULL) {
            ELOG("\n");
            ELOG("****************************************************************\n");
            ELOG("NOTE: Automatically converted configuration file from v3 to v4.\n");
            ELOG("\n");
            ELOG("Please convert your config file to v4. You can use this command:\n");
            ELOG("    mv %s %s.O\n", f, f);
            ELOG("    i3-migrate-config-to-v4 %s.O > %s\n", f, f);
            ELOG("****************************************************************\n");
            ELOG("\n");
            free(new);
            new = converted;
        } else {
            printf("\n");
            printf("**********************************************************************\n");
            printf("ERROR: Could not convert config file. Maybe i3-migrate-config-to-v4\n");
            printf("was not correctly installed on your system?\n");
            printf("**********************************************************************\n");
            printf("\n");
        }
    }


    context = scalloc(sizeof(struct context));
    context->filename = f;

    struct ConfigResult *config_output = parse_config(new, context);
    yajl_gen_free(config_output->json_gen);

    check_for_duplicate_bindings(context);

    if (context->has_errors || context->has_warnings) {
        ELOG("FYI: You are using i3 version " I3_VERSION "\n");
        if (version == 3)
            ELOG("Please convert your configfile first, then fix any remaining errors (see above).\n");

        char *editaction,
             *pageraction;
        sasprintf(&editaction, "i3-sensible-editor \"%s\" && i3-msg reload\n", f);
        sasprintf(&pageraction, "i3-sensible-pager \"%s\"\n", errorfilename);
        char *argv[] = {
            NULL, /* will be replaced by the executable path */
            "-f",
            config.font.pattern,
            "-t",
            (context->has_errors ? "error" : "warning"),
            "-m",
            (context->has_errors ?
             "You have an error in your i3 config file!" :
             "Your config is outdated. Please fix the warnings to make sure everything works."),
            "-b",
            "edit config",
            editaction,
            (errorfilename ? "-b" : NULL),
            (context->has_errors ? "show errors" : "show warnings"),
            pageraction,
            NULL
        };

        start_nagbar(&config_error_nagbar_pid, argv);
        free(editaction);
        free(pageraction);
    }

    FREE(context->line_copy);
    free(context);
    free(new);
    free(buf);

    while (!SLIST_EMPTY(&variables)) {
        current = SLIST_FIRST(&variables);
        FREE(current->key);
        FREE(current->value);
        SLIST_REMOVE_HEAD(&variables, variables);
        FREE(current);
    }
}

#endif
