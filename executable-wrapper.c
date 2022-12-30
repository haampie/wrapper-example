#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum {
  parser_token_start,
  parser_identifier,
  parser_string_end,
  parser_comment,
  identifier_or_delimited_string_quote,
  parser_delimited_string_start,
  parser_inline_delimited_string,
  parser_inline_delimited_string_end,
} state_t;

typedef enum {
  identifier,
  string,
  end_of_command,
  end_of_file,
  fatal_error,
} token_t;

struct parser_t {
  state_t state;
  char closing_delimiter;
  size_t token_start;
  size_t token_end;
  size_t heredoc_start;
  size_t heredoc_size;
  size_t heredoc_pos;
  size_t index;
  int end_of_file;
  token_t token;
};

static void init_parser(struct parser_t *p) {
  p->state = parser_token_start;
  p->index = 0;
  p->end_of_file = 0;
}

static void next_token(struct parser_t *parser, char *input, size_t n) {
  while (1) {
    char c;
    if (parser->index < n) {
      c = input[parser->index];
    } else {
      parser->end_of_file = 1;
    }

    switch (parser->state) {

    case parser_token_start: {
      if (parser->end_of_file) {
        parser->token = end_of_file;
        return;
      }

      // Ignore comments
      if (c == '#') {
        parser->state = parser_comment;
        ++parser->index;
        continue;
      }

      // Begin of a string
      if (c == '"') {
        parser->state = parser_string_end;
        parser->token_start = parser->index + 1;
        ++parser->index;
        continue;
      }

      // Either the start of a string r"(...)" or just an identifier.
      if (c == 'r') {
        parser->state = identifier_or_delimited_string_quote;
        parser->token_start = parser->index;
        ++parser->index;
        continue;
      }

      // End of a command
      if (c == '\n') {
        ++parser->index;
        parser->token = end_of_command;
        return;
      }

      // Ignore whitespace
      if (isspace(c)) {
        ++parser->index;
        continue;
      }

      // Anything else is valid identifier
      parser->state = parser_identifier;
      parser->token_start = parser->index;
      ++parser->index;
      break;
    }

    case parser_identifier: {
      // emit end of identifier
      if (parser->end_of_file || c == '\n') {
        parser->token = identifier;
        parser->state = parser_token_start;
        parser->token_end = parser->index;
        return;
      }

      // emit identifier and step
      if (isspace(c)) {
        parser->token = identifier;
        parser->state = parser_token_start;
        parser->token_end = parser->index;
        ++parser->index;
        return;
      }

      // part of the identifier
      ++parser->index;
      break;
    }

    case parser_comment: {
      // comment end
      if (parser->end_of_file) {
        parser->token = end_of_file;
        return;
      }

      // comment end
      if (c == '\n') {
        parser->token = end_of_command;
        parser->state = parser_token_start;
        ++parser->index;
        return;
      }

      // consume comment
      ++parser->index;
      break;
    }

    case parser_string_end: {
      // unexpected end of file
      if (parser->end_of_file) {
        puts("Unexpected end of file while parsing string\n");
        parser->token = fatal_error;
        return;
      }

      if (c == '"') {
        parser->state = parser_token_start;
        parser->token = string;
        parser->token_end = parser->index;
        ++parser->index;
        return;
      }

      // string continues;
      ++parser->index;
      break;
    }

    case identifier_or_delimited_string_quote: {
      // end of an identifier
      if (parser->end_of_file || c == '\n') {
        parser->state = parser_token_start;
        parser->token = identifier;
        parser->token_end = parser->index;
        return;
      }

      // end of identifier
      if (isspace(c)) {
        parser->state = parser_token_start;
        parser->token = identifier;
        parser->token_end = parser->index;
        ++parser->index;
        return;
      }

      // begin of r"(...)" delimited string
      if (c == '"') {
        parser->state = parser_delimited_string_start;
        parser->heredoc_size = 0;
        parser->heredoc_start = parser->index + 1;
        ++parser->index;
        continue;
      }

      // identifier
      parser->state = parser_identifier;
      ++parser->index;
      break;
    }

    case parser_delimited_string_start: {
      // unexpected end of file
      if (parser->end_of_file) {
        puts("Unexpected end of file while parsing delimited string\n");
        parser->token = fatal_error;
        return;
      }

      // parse r"{...
      if (c == '{' || c == '[' || c == '(' || c == '<') {
        parser->state = parser_inline_delimited_string;
        parser->closing_delimiter = c == '{' ? '}' : c == '[' ? ']' : c == '(' ? ')' : '>';
        parser->token_start = parser->index + 1;
        ++parser->index;
        continue;
      }

      ++parser->heredoc_size;
      ++parser->index;
      continue;
    }

    case parser_inline_delimited_string: {
      // unexpected end of file
      if (parser->end_of_file) {
        puts("Unexpected end of file while parsing delimited string\n");
        parser->token = fatal_error;
        return;
      }

      // closing delimiter
      if (c == parser->closing_delimiter) {
        parser->state = parser_inline_delimited_string_end;
        parser->heredoc_pos = 0;
        parser->token_end = parser->index;
        ++parser->index;
        continue;
      }

      ++parser->index;
      break;
    }

    case parser_inline_delimited_string_end: {
      if (parser->end_of_file) {
        puts("Unexpected end of file while parsing delimited string");
        parser->token = fatal_error;
        return;
      }


      if (parser->heredoc_pos < parser->heredoc_size) {
        // Matching heredoc character first.
        if (c == input[parser->heredoc_start + parser->heredoc_pos]) {
          ++parser->heredoc_pos;
          ++parser->index;
          continue;
        }

        // Mismatch! Reset heredoc pos.
        parser->heredoc_pos = 0;

        // Are we again in string closing mode
        if (c == parser->closing_delimiter) {
          ++parser->index;
          continue;
        }

        // If not, it's string territory.
        parser->state = parser_inline_delimited_string;
        ++parser->index;
        continue;
      }

      // Finished the heredoc
      if (c == '"') {
        parser->state = parser_token_start;
        parser->token = string;
        ++parser->index;
        return;
      }

      // Repeated closing character, maybe the next one ends the string.
      if (c == parser->closing_delimiter) {
        ++parser->index;
        continue;
      }

      // still a string.
      parser->state = parser_inline_delimited_string;
      ++parser->index;
      break;
    }
    }
  }
}

typedef enum {
  command_append,
  command_prepend,
  command_set,
  command_unknown
} command_name_t;

command_name_t parse_command(char const *start, char const *end) {
  size_t len = end - start;
  if (len == 6 && strncmp(start, "append", 6) == 0) {
    return command_append;
  }

  if (len == 7 && strncmp(start, "prepend", 7) == 0) {
    return command_prepend;
  }

  if (len == 3 && strncmp(start, "set", 3) == 0) {
    return command_set;
  }

  return command_unknown;
}

static int execute(char *program, size_t n) {
  char *variable_start, *variable_end;
  char *delim_start, *delim_end;
  char *value_start, *value_end;
  char old_variable_end;
  char old_delim_end;
  char old_value_end;
  char *old_env;
  size_t old_len;
  size_t delim_len;
  size_t value_len;
  size_t new_len;

  struct parser_t parser;
  init_parser(&parser);

  while (1) {
    // Parse a new command.
    next_token(&parser, program, n);

    if (parser.token == fatal_error)
      return 1;

    if (parser.token == end_of_file)
      return 0;

    // Skip over lines without commands
    if (parser.token == end_of_command)
      continue;
    
    // Commands are identifiers
    if (parser.token != identifier) {
      printf("Expected a command got %d\n", parser.token);
      return 1;
    }

    // Identifier should be a command
    command_name_t cmd =
        parse_command(program + parser.token_start, program + parser.token_end);
    switch (cmd) {
    case command_append:
      next_token(&parser, program, n);
      if (parser.token != identifier && parser.token != string)
        return 1;
      variable_start = program + parser.token_start;
      variable_end = program + parser.token_end;
      next_token(&parser, program, n);
      if (parser.token != identifier && parser.token != string)
        return 1;
      delim_start = program + parser.token_start;
      delim_end = program + parser.token_end;
      next_token(&parser, program, n);
      if (parser.token != identifier && parser.token != string)
        return 1;
      value_start = program + parser.token_start;
      value_end = program + parser.token_end;
      next_token(&parser, program, n);
      if (parser.token != end_of_command && parser.token != end_of_file)
        return 1;

      // temporarily put some 0 there.
      old_variable_end = *variable_end;
      old_delim_end = *delim_end;
      old_value_end = *value_end;

      // c-stringify
      *variable_end = 0;
      *delim_end = 0;
      *value_end = 0;

      old_env = getenv(variable_start);
      if (old_env == NULL) {
        setenv(variable_start, value_start, 1);
      } else {
        old_len = strlen(old_env);
        delim_len = delim_end - delim_start;
        value_len = value_end - value_start;
        new_len = old_len + delim_len + value_len;
        char *concatenated = malloc(new_len + 1); // trailing null
        if (concatenated == NULL) return 1;
        memcpy(concatenated, old_env, old_len);
        memcpy(concatenated + old_len, delim_start, delim_len);
        memcpy(concatenated + old_len + delim_len, value_start,
               value_len + 1); // null
        setenv(variable_start, concatenated, 1);
        free(concatenated);
      }

      // undo c-stringification
      *variable_end = old_variable_end;
      *delim_end = old_delim_end;
      *value_end = old_value_end;
      break;

    case command_prepend:
      next_token(&parser, program, n);
      if (parser.token != identifier && parser.token != string)
        return 1;
      variable_start = program + parser.token_start;
      variable_end = program + parser.token_end;
      next_token(&parser, program, n);
      if (parser.token != identifier && parser.token != string)
        return 1;
      delim_start = program + parser.token_start;
      delim_end = program + parser.token_end;
      next_token(&parser, program, n);
      if (parser.token != identifier && parser.token != string)
        return 1;
      value_start = program + parser.token_start;
      value_end = program + parser.token_end;
      next_token(&parser, program, n);
      if (parser.token != end_of_command && parser.token != end_of_file)
        return 1;

      // temporarily put some 0 there.
      old_variable_end = *variable_end;
      old_delim_end = *delim_end;
      old_value_end = *value_end;

      // c-stringify
      *variable_end = 0;
      *delim_end = 0;
      *value_end = 0;

      old_env = getenv(variable_start);
      if (old_env == NULL) {
        setenv(variable_start, value_start, 1);
      } else {
        old_len = strlen(old_env);
        delim_len = delim_end - delim_start;
        value_len = value_end - value_start;
        new_len = old_len + delim_len + value_len;
        char *concatenated = malloc(new_len + 1); // trailing null
        if (concatenated == NULL) return 1;
        memcpy(concatenated, value_start, value_len);
        memcpy(concatenated + value_len, delim_start, delim_len);
        memcpy(concatenated + value_len + delim_len, old_env,
               old_len + 1); // null
        setenv(variable_start, concatenated, 1);
        free(concatenated);
      }

      // undo c-stringification
      *variable_end = old_variable_end;
      *delim_end = old_delim_end;
      *value_end = old_value_end;
      break;

    case command_set:
      next_token(&parser, program, n);
      if (parser.token != identifier && parser.token != string)
        return 1;
      variable_start = program + parser.token_start;
      variable_end = program + parser.token_end;
      next_token(&parser, program, n);
      if (parser.token != identifier && parser.token != string)
        return 1;
      value_start = program + parser.token_start;
      value_end = program + parser.token_end;
      next_token(&parser, program, n);
      if (parser.token != end_of_command && parser.token != end_of_file)
        return 1;

      old_variable_end = *variable_end;
      old_value_end = *value_end;
      *variable_end = 0;
      *value_end = 0;
      setenv(variable_start, value_start, 1);
      *variable_end = old_variable_end;
      *value_end = old_value_end;
      break;

    case command_unknown:
      puts("Unknown commmand");
      return 1;
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 1;

  // The wrapper file
  char *name = argv[1];
  FILE *f = fopen(name, "rb");
  if (f == NULL)
    return 1;

  // Get the real exe: [wrapper]-real
  char real_exe[4096];
  int file_len = strlen(name);
  memcpy(real_exe, name, file_len);
  strcpy(real_exe + file_len, "-real");

  // Read file into memory
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *str = malloc(fsize + 1);
  if (str == NULL) return 1;
  long num_read = fread(str, 1, fsize, f);
  if (num_read != fsize) return 2;
  fclose(f);
  str[fsize] = 0;

  int result = execute(str, fsize);
  if (result != 0)
    return result;

  int exec_result = execv(real_exe, argv + 1);

  printf("Could not execute %s\n", real_exe);

  return exec_result;
}