#include "core.h"
#include <stdio.h>
#include <string.h>
#include <raylib.h>

#define WINDOW_WIDTH     600
#define WINDOW_HEIGHT    400
#define MAX_QUERY_LENGTH 32
#define MAX_ENTRIES      256
#define PADDING_LEFT     15
#define PADDING_RIGHT    15
#define PADDING_UP_DOWN  50
#define ROW_WIDTH        (WINDOW_WIDTH - PADDING_LEFT - PADDING_RIGHT)
#define ROW_HEIGHT       32
#define ROW_GAP          4
#define FONT_SIZE        20
#define HEADER_HEIGHT    40

typedef struct {
  Str name;
  Str description;
  Str command;
  size_t fuzzy_score;
} Entry;

typedef struct {
  Color background; 
  Color text_color;
  Color box_bg_color;
  Color selected_box_color;
} Config;

static struct {
  Allocator allocator;
  // The config simply loads the theme and other things that change the UI for
  // the user
  Config config;
  // The entries are either loaded from a file or passed as input parameters
  // for trying to make a dmenu replacement
  Entry entries[MAX_ENTRIES];
  size_t num_entries;
  // The entries are loaded from a file, since they are views we also store the
  // contents of the loaded file to free at the end
  Str entries_file;
  // Index of the currently selected entry (the one that gets executed if the
  // user presses enter)
  size_t selected_entry;
  // The state of the query is stored as a simple char array
  size_t query_len;
  char query[MAX_QUERY_LENGTH];
} STATE = { 0 };

static ErrorCode LoadThemeFromFile(void)
{
  ErrorCode ret_code = kErrorNone;
  Str contents;
  TRY(LoadFileAsStr(&STATE.allocator, THEME_FILE_PATH, &contents));
  Str tofree = contents;

  DEALLOCATE(&STATE.allocator, (void *)tofree.data, tofree.length); 
  return ret_code;
}

static ErrorCode LoadEntriesFromFile(void)
{
  ErrorCode ret_code = kErrorNone;
  Str contents;
  TRY(LoadFileAsStr(&STATE.allocator, ENTRIES_FILE_PATH, &contents));
  STATE.entries_file = contents;

  while (contents.length > 0 && ret_code == kErrorNone) {
    Str line = StrTrim(StrChopLeft(&contents, '\n'));
    if (line.length == 0) { continue; }
    if (line.data[0] == '#') { continue; }
    if (StrCountChar(line, ',') != 2) { ret_code = kErrorParsing; }
    Str name = StrTrim(StrChopLeft(&line, ','));
    Str cmd  = StrTrim(StrChopLeft(&line, ','));
    Str desc = StrTrim(line);
    STATE.entries[STATE.num_entries++] = (Entry) { name, desc, cmd, 0 };
  }

  return ret_code;
}

static ErrorCode LoadEntriesFromStdin(void)
{
  void *raw;
  size_t size = 0;
  size_t capacity = 4096;
  TRY(ALLOCATE(&STATE.allocator, &raw, capacity * sizeof(char), 1));

  while (!feof(stdin)) {
    if (size == capacity) {
      size_t new_capacity = capacity * 2;
      TRY(REALLOCATE(&STATE.allocator, &raw, capacity * sizeof(char),
            new_capacity * sizeof(char), 1));
      capacity = new_capacity;
    }
    size_t n = fread((char *)raw + size, 1, capacity - size, stdin);
    if (n == 0) { break; }
    size += n;

    if (ferror(stdin)) {
      DEALLOCATE(&STATE.allocator, raw, capacity * sizeof(char));
      return kErrorOS;
    }
  }

  Str contents = StrFromParts((const char *)raw, size);
  STATE.entries_file = contents;
  while (contents.length > 0) {
    if (STATE.num_entries == MAX_ENTRIES) { return kErrorOutOfMem; }
    Str token = StrChopLeft(&contents, '\n'); 
    STATE.entries[STATE.num_entries++].name = token;
  }
  return kErrorNone; 
}

static void RunCommand(Str command)
{
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "%.*s", (int)command.length, command.data);
  StartCommand(cmd);
}

static bool HandleInputs(bool *pressed_enter)
{
    bool selection_dirty = false;
    char c;

    while (((c = GetCharPressed()) != 0)
        && STATE.query_len < MAX_QUERY_LENGTH - 1) { 
      STATE.query[STATE.query_len++] = (char)c;
      STATE.query[STATE.query_len] = '\0';
      selection_dirty = true;
    }

    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE))
        && STATE.query_len > 0) {
      STATE.query[--STATE.query_len] = '\0';
      selection_dirty = true;
    }

    if ((IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN))
        && STATE.selected_entry + 1 < STATE.num_entries) {
      STATE.selected_entry++;
    }

    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
      STATE.selected_entry = STATE.selected_entry > 0
        ? STATE.selected_entry - 1
        : 0;
    }

    if (IsKeyPressed(KEY_ENTER)) {
      *pressed_enter = true;
    }

    return selection_dirty;
}

static int QSortCmp(const void *a, const void *b)
{
  Entry sa = *((Entry *)a);
  Entry sb = *((Entry *)b);
  return sb.fuzzy_score - sa.fuzzy_score;
}

static void RecomputeFuzzyScores(void)
{
  for (size_t i = 0; i < STATE.num_entries; i++) {
    STATE.entries[i].fuzzy_score = 0;
    for (size_t i = 0; i < STATE.query_len; i++) {
      STATE.entries[i].fuzzy_score += StrCountChar(STATE.entries[i].name,
          STATE.query[i]);
    }
  }
  qsort(STATE.entries, STATE.num_entries, sizeof(Entry), QSortCmp); 
}

static void RenderHeader(void)
{
  DrawRectangle(0, 0, WINDOW_WIDTH, HEADER_HEIGHT, STATE.config.box_bg_color);
  DrawText(TextFormat("> %s_", STATE.query), PADDING_LEFT,
      (HEADER_HEIGHT - 24) / 2, 24, STATE.config.text_color);
}

static void RenderEntries(void)
{
  int x = (WINDOW_WIDTH - ROW_WIDTH) / 2;

  for (size_t i = 0; i < STATE.num_entries; i++) {
    int y = PADDING_UP_DOWN + i * ROW_HEIGHT;
    Color box = (i == STATE.selected_entry)
      ? STATE.config.selected_box_color
      : STATE.config.box_bg_color;

    DrawRectangle(x, y, ROW_WIDTH, ROW_HEIGHT - ROW_GAP, box);

    DrawText(TextFormat("%.*s", (int)STATE.entries[i].name.length,
          STATE.entries[i].name.data), x + 8,
          y + (ROW_HEIGHT - ROW_GAP - FONT_SIZE) / 2, FONT_SIZE,
          STATE.config.text_color);
  }
}

int main(int argc, const char *argv[])
{
  ErrorCode err;
  bool chosen = false;
  STATE.allocator = GetLibcAllocator();

  if ((err = LoadThemeFromFile()) != kErrorNone) {
    STATE.config.background         = (Color){ 30,   30,  34, 255 };
    STATE.config.text_color         = (Color){ 220, 220, 225, 255 };
    STATE.config.box_bg_color       = (Color){ 44,   44,  52, 255 };
    STATE.config.selected_box_color = (Color){ 70,  110, 180, 255 };
  }

  if (argc > 1) {
    if (strcmp(argv[1], "dmenu") == 0) {
      if ((err = LoadEntriesFromStdin()) != kErrorNone) {
        fprintf(stderr, "Couldn't load the entries from stdin\n");
        return 1;
      } 
    } else {
      fprintf(stderr, "Unknown mode %s\n", argv[1]);
      return 1;
    }
  } else {
    if ((err = LoadEntriesFromFile()) != kErrorNone) {
      fprintf(stderr, "Couldn't open and parse entries file - %s\n",
          ErrorCodeToString(err));
      return 1;
    }
  }

  SetTraceLogLevel(LOG_NONE);
  InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "chooseit");
  SetTargetFPS(60);

  while (!WindowShouldClose() && !chosen) {
    if (HandleInputs(&chosen)) { RecomputeFuzzyScores(); }

    BeginDrawing();
    ClearBackground(STATE.config.background);
    RenderHeader();
    RenderEntries();
    EndDrawing();
  }

  if (chosen) {
    if (argc > 1) {
      printf("%.*s", (int)STATE.entries[STATE.selected_entry].name.length,
          STATE.entries[STATE.selected_entry].name.data);
    } else {
      RunCommand(STATE.entries[STATE.selected_entry].command);
    }
  }
  CloseWindow();
  DEALLOCATE(&STATE.allocator, (void *)STATE.entries_file.data,
      STATE.entries_file.length);
}
