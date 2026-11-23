#include "net/roast_dropbox.h"
#include <cassert>
#include <cstdlib>
#include <cstring>

static bool count(const rn_dbx_entry_t *entry, void *ctx) {
    assert(!strcmp(entry->name, "curve.json"));
    ++*static_cast<int *>(ctx);
    return true;
}
static void parse(const char *text, bool expected) {
    FILE *page = tmpfile();
    assert(page);
    fputs(text, page);
    rewind(page);
    char *cursor = nullptr;
    int entries = 0;
    assert(rn_dbx_parse_page(page, count, &entries, &cursor) == expected);
    assert(!cursor);
    fclose(page);
}
int main() {
    FILE *page = tmpfile();
    assert(page);
    fputs("{\"entries\":[", page);
    for (int i=0; i<1000; ++i) {
        if (i) fputc(',', page);
        fputs("{\"name\":\"curve.json\",\"sharing_info\":{\"name\":\"ignored\"},\".tag\":\"file\"}", page);
    }
    fputs("],\"has_more\":true,\"cursor\":\"next-page\"}", page);
    rewind(page);
    char *cursor = nullptr;
    int n = 0;
    assert(rn_dbx_parse_page(page, count, &n, &cursor));
    assert(n == 1000 && !strcmp(cursor, "next-page"));
    free(cursor);
    fclose(page);
    parse("{\"entries\":[],\"has_more\":false}", true);
    parse("{\"entries\":[],\"has_more\":true}", false);
    parse("{\"has_more\":false}", false);
    parse("{\"entries\":[{\".tag\":\"file\",\"name\":\"curve.json\"},", false);
    parse("{\"entries\":[{\".tag\":\"file\",\"name\":\"../curve.json\"}],\"has_more\":false}", false);
}
