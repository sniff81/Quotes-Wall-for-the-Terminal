#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curses.h>
#include <time.h>
#include <ctype.h>

typedef struct {
    char *author;
    char *quote;
} QuoteItem;

#define MAX_ROW_QUOTES 32
#define AUTHOR_MAX 511
#define QUOTE_MAX 2047

typedef struct {
    int quote_index;
    size_t total_len;
    size_t author_len;
} StreamItem;

typedef struct {
    StreamItem items[MAX_ROW_QUOTES];
    int count;
    size_t offset;          /* Character position into the first quote */
    int speed_interval;
    int speed_counter;
} RowStream;

/* Read a JSON string, handling common escaped characters. */
static char *parse_json_string(const char *p, const char *limit, char *out,
                               size_t out_size)
{
    size_t n = 0;
    while (p < limit && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\' && p < limit) {
            char e = *p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                default: c = (unsigned char)e; break;
            }
        }
        if (n + 1 < out_size)
            out[n++] = (char)c;
    }
    out[n] = '\0';
    return (char *)p;
}

static int extract_field(const char *obj, const char *end,
                         const char *key, char *out, size_t out_size)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = obj;

    while (p < end) {
        const char *found = strstr(p, pattern);
        if (!found || found >= end)
            return 0;

        const char *colon = strchr(found + strlen(pattern), ':');
        if (!colon || colon >= end)
            return 0;

        const char *value = colon + 1;
        while (value < end && isspace((unsigned char)*value))
            value++;

        if (value >= end || *value != '"') {
            p = found + strlen(pattern);
            continue;
        }

        value++;
        const char *after = parse_json_string(value, end, out, out_size);
        return after < end || (after == end && end[-1] == '"');
    }
    return 0;
}

/* Loads quotes once. The parser supports ordinary JSON quote/author strings. */
static int load_quotes(const char *filename, QuoteItem **out_quotes, int *out_count)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        perror(filename);
        return 0;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long file_len = ftell(f);
    if (file_len < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    char *buffer = malloc((size_t)file_len + 1);
    if (!buffer) {
        fclose(f);
        return 0;
    }

    size_t got = fread(buffer, 1, (size_t)file_len, f);
    buffer[got] = '\0';
    fclose(f);

    int capacity = 256, count = 0;
    QuoteItem *quotes = calloc((size_t)capacity, sizeof(*quotes));
    if (!quotes) {
        free(buffer);
        return 0;
    }

    const char *p = buffer;
    const char *file_end = buffer + got;
    while (p < file_end) {
        const char *start = strchr(p, '{');
        if (!start)
            break;
        const char *end = strchr(start + 1, '}');
        if (!end)
            break;

        char author[AUTHOR_MAX + 1] = {0};
        char quote[QUOTE_MAX + 1] = {0};

        int has_quote = extract_field(start, end, "quote", quote, sizeof(quote));
        int has_author = extract_field(start, end, "author", author, sizeof(author));

        if (has_quote && quote[0]) {
            if (!has_author || !author[0])
                snprintf(author, sizeof(author), "UNKNOWN");

            if (count == capacity) {
                int new_capacity = capacity * 2;
                QuoteItem *tmp = realloc(quotes, (size_t)new_capacity * sizeof(*quotes));
                if (!tmp)
                    break;
                quotes = tmp;
                capacity = new_capacity;
            }

            quotes[count].author = strdup(author);
            quotes[count].quote = strdup(quote);
            if (!quotes[count].author || !quotes[count].quote) {
                free(quotes[count].author);
                free(quotes[count].quote);
                break;
            }
            count++;
        }
        p = end + 1;
    }

    free(buffer);
    *out_quotes = quotes;
    *out_count = count;
    return count > 0;
}

static size_t quote_length(const QuoteItem *q)
{
    /* Leading space + author + ": " + quote + four trailing spaces. */
    return 1 + strlen(q->author) + 2 + strlen(q->quote) + 4;
}

static void append_quote(RowStream *row, int total_quotes, QuoteItem *quotes)
{
    if (row->count >= MAX_ROW_QUOTES || total_quotes <= 0)
        return;

    int idx = rand() % total_quotes;
    StreamItem *item = &row->items[row->count++];
    item->quote_index = idx;
    item->author_len = strlen(quotes[idx].author);
    item->total_len = quote_length(&quotes[idx]);
}

static void init_row_stream(RowStream *row, int total_quotes, QuoteItem *quotes)
{
    memset(row, 0, sizeof(*row));
    row->speed_interval = (rand() % 3) + 1;
    row->speed_counter = 0;

    while (row->count < MAX_ROW_QUOTES)
        append_quote(row, total_quotes, quotes);

    /* Stagger rows so they do not all start at the same point. */
    if (row->count > 0)
        row->offset = (size_t)(rand() % (int)(row->items[0].total_len + 1));
}

/* Return the character and color at an index within one formatted quote. */
static char quote_char(const QuoteItem *q, size_t pos, int *is_author)
{
    size_t author_len = strlen(q->author);
    size_t quote_start = 1 + author_len + 2;

    if (pos == 0) {
        *is_author = 1;
        return ' ';
    }
    if (pos <= author_len) {
        *is_author = 1;
        return q->author[pos - 1];
    }
    if (pos <= author_len + 2) {
        *is_author = 1;
        return pos == author_len + 1 ? ':' : ' ';
    }
    if (pos < quote_start + strlen(q->quote)) {
        *is_author = 0;
        return q->quote[pos - quote_start];
    }
    *is_author = 0;
    return ' ';
}

static void draw_row(int y, int width, RowStream *row, QuoteItem *quotes)
{
    int item_index = 0;
    size_t pos_in_item = row->offset;

    /* Find the item containing the leftmost visible character. */
    while (item_index < row->count &&
           pos_in_item >= row->items[item_index].total_len) {
        pos_in_item -= row->items[item_index].total_len;
        item_index++;
    }

    move(y, 0);
    clrtoeol();

    for (int x = 0; x < width; x++) {
        while (item_index < row->count &&
               pos_in_item >= row->items[item_index].total_len) {
            pos_in_item -= row->items[item_index].total_len;
            item_index++;
        }

        if (item_index >= row->count) {
            mvaddch(y, x, ' ');
            continue;
        }

        StreamItem *item = &row->items[item_index];
        QuoteItem *q = &quotes[item->quote_index];
        int is_author = 0;
        char c = quote_char(q, pos_in_item, &is_author);

        attron(COLOR_PAIR(is_author ? 1 : 2));
        mvaddch(y, x, (unsigned char)c);
        attroff(COLOR_PAIR(is_author ? 1 : 2));
        pos_in_item++;
    }
}

static void advance_row(RowStream *row, int total_quotes, QuoteItem *quotes)
{
    row->speed_counter++;
    if (row->speed_counter < row->speed_interval)
        return;

    row->speed_counter = 0;
    row->offset++;

    /* Retire a quote only after its final character has left the screen. */
    while (row->count > 0 && row->offset >= row->items[0].total_len) {
        row->offset -= row->items[0].total_len;

        for (int i = 1; i < row->count; i++)
            row->items[i - 1] = row->items[i];
        row->count--;

        /* Keep the queue full: one random quote is appended at the tail. */
        append_quote(row, total_quotes, quotes);
    }

    /* Defensive refill if the queue was ever left short. */
    while (row->count < MAX_ROW_QUOTES)
        append_quote(row, total_quotes, quotes);
}

int main(int argc, char **argv)
{
    srand((unsigned int)time(NULL));

    const char *filename = argc > 1 ? argv[1] : "quotes.json";
    QuoteItem *quotes = NULL;
    int total_quotes = 0;

    if (!load_quotes(filename, &quotes, &total_quotes) || total_quotes == 0) {
        fprintf(stderr, "Failed to load quotes from %s or file is empty.\n", filename);
        if (quotes) {
            for (int i = 0; i < total_quotes; i++) {
                free(quotes[i].author);
                free(quotes[i].quote);
            }
            free(quotes);
        }
        return 1;
    }

    initscr();
    noecho();
    cbreak();
    curs_set(0);
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);

    if (has_colors()) {
        start_color();
        use_default_colors();
        if (can_change_color()) {
            init_color(8, 933, 933, 933);
            init_color(9, 604, 804, 196);
            init_pair(1, 9, -1);
            init_pair(2, 8, -1);
        } else {
            init_pair(1, COLOR_GREEN, -1);
            init_pair(2, COLOR_WHITE, -1);
        }
    } else {
        init_pair(1, COLOR_WHITE, -1);
        init_pair(2, COLOR_WHITE, -1);
    }

    int old_y = -1, old_x = -1;
    RowStream *rows = NULL;
    int running = 1;

    while (running) {
        int height, width;
        getmaxyx(stdscr, height, width);

        if (height != old_y || width != old_x) {
            free(rows);
            rows = calloc((size_t)height, sizeof(*rows));
            if (!rows) {
                endwin();
                fprintf(stderr, "Out of memory allocating terminal rows.\n");
                for (int i = 0; i < total_quotes; i++) {
                    free(quotes[i].author);
                    free(quotes[i].quote);
                }
                free(quotes);
                return 1;
            }
            for (int y = 0; y < height; y++)
                init_row_stream(&rows[y], total_quotes, quotes);
            old_y = height;
            old_x = width;
            clear();
        }

        for (int y = 0; y < height; y++) {
            draw_row(y, width, &rows[y], quotes);
            advance_row(&rows[y], total_quotes, quotes);
        }

        refresh();
        int ch = getch();
        if (ch == 'q' || ch == 'Q')
            running = 0;

        usleep(120000);
    }

    free(rows);
    for (int i = 0; i < total_quotes; i++) {
        free(quotes[i].author);
        free(quotes[i].quote);
    }
    free(quotes);
    endwin();
    return 0;
}
