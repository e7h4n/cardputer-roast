// SPDX-License-Identifier: MIT
#include "roast_dropbox.h"
#include <rapidjson/filereadstream.h>
#include <rapidjson/reader.h>
#include <cstdlib>
#include <cstring>

namespace {
// A page can contain many entries. Retain fields only for the current direct
// child of "entries"; ignore nested sharing and media metadata.
struct Page : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, Page> {
    rn_dbx_entry_cb emit;
    void *ctx;
    int depth = 0;
    bool entries = false, sawEntries = false, sawMore = false, more = false;
    enum Field { Other, Entries, More, Cursor, Tag, Name, Hash } field = Other;
    rn_dbx_entry_t entry{};
    bool file = false, tagged = false, validName = false;
    char *cursor = nullptr;
    Page(rn_dbx_entry_cb cb, void *arg) : emit(cb), ctx(arg) {}
    ~Page() { free(cursor); }

    bool Key(const char *key, rapidjson::SizeType, bool) {
        field = Other;
        if (depth == 1) {
            if (!strcmp(key, "entries")) field = Entries;
            else if (!strcmp(key, "has_more")) field = More;
            else if (!strcmp(key, "cursor")) field = Cursor;
        } else if (entries && depth == 3) {
            if (!strcmp(key, ".tag")) field = Tag;
            else if (!strcmp(key, "name")) field = Name;
            else if (!strcmp(key, "content_hash")) field = Hash;
        }
        return true;
    }
    bool StartArray() {
        if (depth == 1 && field == Entries) {
            if (sawEntries) return false;
            entries = sawEntries = true;
        } else if (!depth || (entries && depth == 2)) return false;
        field = Other;
        ++depth;
        return true;
    }
    bool EndArray(rapidjson::SizeType) {
        if (--depth == 1) entries = false;
        return true;
    }
    bool StartObject() {
        if (entries && depth == 2) {
            entry = {};
            file = tagged = validName = false;
        }
        field = Other;
        ++depth;
        return true;
    }
    bool EndObject(rapidjson::SizeType) {
        --depth;
        if (!entries || depth != 2) return true;
        return tagged && (!file || (validName && emit(&entry, ctx)));
    }
    bool Default() { return !(entries && depth == 2); }
    bool Bool(bool v) {
        if (depth == 1 && field == More) {
            sawMore = true;
            more = v;
        }
        return Default();
    }
    bool String(const char *s, rapidjson::SizeType len, bool) {
        if (field == Cursor && depth == 1) {
            if (strlen(s) != len || cursor) return false;
            cursor = strdup(s);
            if (!cursor) return false;
        } else if (entries && depth == 3) {
            if (field == Tag) {
                tagged = true;
                file = !strcmp(s, "file");
            } else if (field == Name) {
                validName = len && len < sizeof(entry.name) && strlen(s) == len &&
                    !strpbrk(s, "/\\:") && strcmp(s, ".") && strcmp(s, "..");
                if (validName) memcpy(entry.name, s, len + 1);
            } else if (field == Hash && len == 64) {
                memcpy(entry.hash, s, 65);
            }
        }
        return Default();
    }
};
}

bool rn_dbx_parse_page(FILE *fp, rn_dbx_entry_cb cb, void *ctx, char **next)
{
    char chunk[512];
    rapidjson::FileReadStream stream(fp, chunk, sizeof chunk);
    rapidjson::Reader reader;
    Page page(cb, ctx);
    *next = nullptr;
    if (!reader.Parse<rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag>(stream, page) ||
        ferror(fp) || !page.sawEntries || !page.sawMore) return false;
    if (page.more) {
        if (!page.cursor || !*page.cursor) return false;
        *next = page.cursor;
        page.cursor = nullptr;
    }
    return true;
}
