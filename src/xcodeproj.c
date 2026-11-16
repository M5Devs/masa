/*
 * xcodeproj.c - build an Xcode project without Xcode.
 *
 * `masa xcode build App.xcodeproj` reads the text-based project.pbxproj
 * (the ASCII plist Xcode writes), pulls out the native target's product
 * name and its C source files, then compiles and links them into a Mach-O
 * executable through the Masa cc wrapper.  With --bundle the result is
 * laid out as a minimal .app directory.
 *
 * The reader is a small recursive-descent parser for the subset of the
 * old-style plist grammar the format uses: dictionaries, arrays and
 * quoted/bare strings, with block and line comments.  It is not a
 * general-purpose plist parser and does not understand data or date
 * literals, which project.pbxproj does not use.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "masa.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MASA_PATH_MAX 4096
#define MASA_CC_MAX_ARGS 8192

/* ------------------------------------------------------------------ */
/* Value tree                                                         */
/* ------------------------------------------------------------------ */

enum pbx_kind {
    PBX_STRING,
    PBX_DICT,
    PBX_ARRAY
};

struct pbx_pair {
    char *key;
    struct pbx_node *value;
};

struct pbx_node {
    enum pbx_kind kind;
    char *string;              /* PBX_STRING */
    struct pbx_pair *pairs;    /* PBX_DICT */
    size_t pair_count;
    struct pbx_node **items;   /* PBX_ARRAY */
    size_t item_count;
};

static void pbx_free(struct pbx_node *node)
{
    size_t i;

    if (node == NULL) {
        return;
    }
    switch (node->kind) {
    case PBX_STRING:
        free(node->string);
        break;
    case PBX_DICT:
        for (i = 0; i < node->pair_count; i++) {
            free(node->pairs[i].key);
            pbx_free(node->pairs[i].value);
        }
        free(node->pairs);
        break;
    case PBX_ARRAY:
        for (i = 0; i < node->item_count; i++) {
            pbx_free(node->items[i]);
        }
        free(node->items);
        break;
    }
    free(node);
}

/* A fresh string node, or NULL on allocation failure. */
static struct pbx_node *pbx_new_string(const char *text)
{
    struct pbx_node *node = calloc(1, sizeof(*node));

    if (node == NULL) {
        return NULL;
    }
    node->string = strdup(text);
    if (node->string == NULL) {
        free(node);
        return NULL;
    }
    node->kind = PBX_STRING;
    return node;
}

static struct pbx_node *pbx_new_container(enum pbx_kind kind)
{
    struct pbx_node *node = calloc(1, sizeof(*node));

    if (node != NULL) {
        node->kind = kind;
    }
    return node;
}

static int pbx_dict_add(struct pbx_node *dict, char *key,
                        struct pbx_node *value)
{
    struct pbx_pair *grown;

    if (dict->pair_count % 8 == 0) {
        size_t next = dict->pair_count + 8;
        grown = realloc(dict->pairs, next * sizeof(*grown));
        if (grown == NULL) {
            return -1;
        }
        dict->pairs = grown;
    }
    dict->pairs[dict->pair_count].key = key;
    dict->pairs[dict->pair_count].value = value;
    dict->pair_count++;
    return 0;
}

static int pbx_array_add(struct pbx_node *array, struct pbx_node *value)
{
    struct pbx_node **grown;

    if (array->item_count % 8 == 0) {
        size_t next = array->item_count + 8;
        grown = realloc(array->items, next * sizeof(*grown));
        if (grown == NULL) {
            return -1;
        }
        array->items = grown;
    }
    array->items[array->item_count++] = value;
    return 0;
}

/* Look up a dictionary member by key; NULL when absent or not a dict. */
static struct pbx_node *pbx_dict_get(const struct pbx_node *dict,
                                     const char *key)
{
    size_t i;

    if (dict == NULL || dict->kind != PBX_DICT) {
        return NULL;
    }
    for (i = 0; i < dict->pair_count; i++) {
        if (strcmp(dict->pairs[i].key, key) == 0) {
            return dict->pairs[i].value;
        }
    }
    return NULL;
}

/* The text of a string member of dict, or NULL. */
static const char *pbx_dict_string(const struct pbx_node *dict,
                                   const char *key)
{
    struct pbx_node *value = pbx_dict_get(dict, key);

    if (value == NULL || value->kind != PBX_STRING) {
        return NULL;
    }
    return value->string;
}

/* ------------------------------------------------------------------ */
/* Parser                                                             */
/* ------------------------------------------------------------------ */

struct pbx_parser {
    const char *p;
    const char *end;
    int failed;
};

static void skip_ws(struct pbx_parser *ps)
{
    for (;;) {
        while (ps->p < ps->end &&
               (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' ||
                *ps->p == '\r')) {
            ps->p++;
        }
        if (ps->p + 1 < ps->end && ps->p[0] == '/' && ps->p[1] == '*') {
            ps->p += 2;
            while (ps->p + 1 < ps->end &&
                   !(ps->p[0] == '*' && ps->p[1] == '/')) {
                ps->p++;
            }
            ps->p = (ps->p + 1 < ps->end) ? ps->p + 2 : ps->end;
            continue;
        }
        if (ps->p + 1 < ps->end && ps->p[0] == '/' && ps->p[1] == '/') {
            while (ps->p < ps->end && *ps->p != '\n') {
                ps->p++;
            }
            continue;
        }
        break;
    }
}

static int is_delim(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '{' || c == '}' || c == '(' || c == ')' ||
           c == '=' || c == ';' || c == ',';
}

/* Parse a quoted or bare string into a heap copy, or NULL on failure. */
static char *parse_string_token(struct pbx_parser *ps)
{
    char *out;
    size_t cap;

    skip_ws(ps);
    if (ps->p >= ps->end) {
        ps->failed = 1;
        return NULL;
    }

    cap = (size_t)(ps->end - ps->p) + 1;
    out = malloc(cap);
    if (out == NULL) {
        ps->failed = 1;
        return NULL;
    }

    if (*ps->p == '"') {
        size_t n = 0;

        ps->p++;
        while (ps->p < ps->end && *ps->p != '"') {
            if (*ps->p == '\\' && ps->p + 1 < ps->end) {
                ps->p++;
                if (*ps->p == 'n') {
                    out[n++] = '\n';
                    ps->p++;
                    continue;
                }
            }
            out[n++] = *ps->p++;
        }
        if (ps->p >= ps->end) {
            free(out);
            ps->failed = 1;
            return NULL;
        }
        ps->p++; /* closing quote */
        out[n] = '\0';
        return out;
    }

    {
        size_t n = 0;

        while (ps->p < ps->end && !is_delim(*ps->p)) {
            out[n++] = *ps->p++;
        }
        out[n] = '\0';
        if (n == 0) {
            free(out);
            ps->failed = 1;
            return NULL;
        }
    }
    return out;
}

static struct pbx_node *parse_value(struct pbx_parser *ps);

static struct pbx_node *parse_dict(struct pbx_parser *ps)
{
    struct pbx_node *dict = pbx_new_container(PBX_DICT);

    if (dict == NULL) {
        ps->failed = 1;
        return NULL;
    }

    ps->p++; /* '{' */

    for (;;) {
        char *key;
        struct pbx_node *value;

        skip_ws(ps);
        if (ps->p >= ps->end) {
            ps->failed = 1;
            break;
        }
        if (*ps->p == '}') {
            ps->p++;
            return dict;
        }

        key = parse_string_token(ps);
        if (key == NULL) {
            break;
        }

        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '=') {
            free(key);
            ps->failed = 1;
            break;
        }
        ps->p++; /* '=' */

        value = parse_value(ps);
        if (value == NULL) {
            free(key);
            break;
        }

        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ';') {
            ps->p++;
        } else {
            pbx_free(value);
            free(key);
            ps->failed = 1;
            break;
        }

        if (pbx_dict_add(dict, key, value) != 0) {
            pbx_free(value);
            free(key);
            ps->failed = 1;
            break;
        }
    }

    pbx_free(dict);
    return NULL;
}

static struct pbx_node *parse_array(struct pbx_parser *ps)
{
    struct pbx_node *array = pbx_new_container(PBX_ARRAY);

    if (array == NULL) {
        ps->failed = 1;
        return NULL;
    }

    ps->p++; /* '(' */

    for (;;) {
        struct pbx_node *value;

        skip_ws(ps);
        if (ps->p >= ps->end) {
            ps->failed = 1;
            break;
        }
        if (*ps->p == ')') {
            ps->p++;
            return array;
        }

        value = parse_value(ps);
        if (value == NULL) {
            break;
        }
        if (pbx_array_add(array, value) != 0) {
            pbx_free(value);
            ps->failed = 1;
            break;
        }

        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
        }
    }

    pbx_free(array);
    return NULL;
}

static struct pbx_node *parse_value(struct pbx_parser *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) {
        ps->failed = 1;
        return NULL;
    }
    if (*ps->p == '{') {
        return parse_dict(ps);
    }
    if (*ps->p == '(') {
        return parse_array(ps);
    }

    {
        char *text = parse_string_token(ps);
        struct pbx_node *node;

        if (text == NULL) {
            return NULL;
        }
        node = pbx_new_string(text);
        free(text);
        if (node == NULL) {
            ps->failed = 1;
        }
        return node;
    }
}

/* ------------------------------------------------------------------ */
/* Source and target extraction                                       */
/* ------------------------------------------------------------------ */

/* True when the reference is a C translation unit. */
static int is_c_source(const char *path)
{
    size_t len;

    if (path == NULL) {
        return 0;
    }
    len = strlen(path);
    return len > 2 && strcmp(path + len - 2, ".c") == 0;
}

/*
 * The objects dictionary: every pbxproj object is a member keyed by its
 * identifier, so the pair list is the object list.
 */
static struct pbx_node *objects_dict(const struct pbx_node *root)
{
    return pbx_dict_get(root, "objects");
}

/*
 * Resolved file references: the object identifier of a PBXFileReference
 * mapped to the path relative to the .xcodeproj parent directory.  Xcode
 * records a file's path relative to its containing group, so the path
 * only makes sense once the group chain has been joined.
 */
struct refmap {
    char **ids;
    char **paths;
    size_t count;
    size_t cap;
};

static void refmap_free(struct refmap *map)
{
    size_t i;

    for (i = 0; i < map->count; i++) {
        free(map->ids[i]);
        free(map->paths[i]);
    }
    free(map->ids);
    free(map->paths);
    map->ids = NULL;
    map->paths = NULL;
    map->count = 0;
    map->cap = 0;
}

static int refmap_add(struct refmap *map, const char *id, const char *path)
{
    char *id_copy;
    char *path_copy;

    if (map->count == map->cap) {
        size_t next = (map->cap == 0) ? 16 : map->cap * 2;
        char **ids = realloc(map->ids, next * sizeof(*ids));
        char **paths;

        if (ids == NULL) {
            return -1;
        }
        map->ids = ids;
        paths = realloc(map->paths, next * sizeof(*paths));
        if (paths == NULL) {
            return -1;
        }
        map->paths = paths;
        map->cap = next;
    }

    id_copy = strdup(id);
    path_copy = strdup(path);
    if (id_copy == NULL || path_copy == NULL) {
        free(id_copy);
        free(path_copy);
        return -1;
    }
    map->ids[map->count] = id_copy;
    map->paths[map->count] = path_copy;
    map->count++;
    return 0;
}

static const char *refmap_lookup(const struct refmap *map, const char *id)
{
    size_t i;

    for (i = 0; i < map->count; i++) {
        if (strcmp(map->ids[i], id) == 0) {
            return map->paths[i];
        }
    }
    return NULL;
}

/* Join a group prefix and a child path with a single separator. */
static char *join_path(const char *prefix, const char *rel)
{
    size_t need;
    char *out;

    if (prefix == NULL || prefix[0] == '\0') {
        need = strlen(rel) + 1;
        out = malloc(need);
        if (out != NULL) {
            snprintf(out, need, "%s", rel);
        }
        return out;
    }

    need = strlen(prefix) + strlen(rel) + 2;
    out = malloc(need);
    if (out != NULL) {
        snprintf(out, need, "%s/%s", prefix, rel);
    }
    return out;
}

/*
 * Walk the group tree under `uuid`, recording each PBXFileReference's
 * path relative to the project directory.  Groups nest, so the prefix
 * grows by each group's `path` component.  depth guards against a
 * malformed project whose group graph contains a cycle.
 */
static int walk_group(const struct pbx_node *objects, const char *uuid,
                      const char *prefix, struct refmap *map, int depth)
{
    struct pbx_node *group;
    struct pbx_node *children;
    const char *isa;
    const char *group_path;
    char *new_prefix;
    size_t i;

    if (depth > 64) {
        return 0;
    }

    group = pbx_dict_get(objects, uuid);
    if (group == NULL || group->kind != PBX_DICT) {
        return 0;
    }
    isa = pbx_dict_string(group, "isa");
    if (isa == NULL) {
        return 0;
    }

    if (strcmp(isa, "PBXFileReference") == 0) {
        const char *path = pbx_dict_string(group, "path");
        char *joined;

        if (path == NULL) {
            path = pbx_dict_string(group, "name");
        }
        if (path == NULL) {
            return 0;
        }
        joined = join_path(prefix, path);
        if (joined == NULL) {
            return -1;
        }
        if (refmap_add(map, uuid, joined) != 0) {
            free(joined);
            return -1;
        }
        free(joined);
        return 0;
    }

    if (strcmp(isa, "PBXGroup") != 0 &&
        strcmp(isa, "PBXVariantGroup") != 0 &&
        strcmp(isa, "XCVersionGroup") != 0) {
        return 0;
    }

    group_path = pbx_dict_string(group, "path");
    if (group_path != NULL && group_path[0] != '\0') {
        new_prefix = join_path(prefix, group_path);
    } else {
        new_prefix = (prefix != NULL) ? strdup(prefix) : strdup("");
    }
    if (new_prefix == NULL) {
        return -1;
    }

    children = pbx_dict_get(group, "children");
    if (children != NULL && children->kind == PBX_ARRAY) {
        for (i = 0; i < children->item_count; i++) {
            if (children->items[i]->kind != PBX_STRING) {
                continue;
            }
            if (walk_group(objects, children->items[i]->string, new_prefix,
                           map, depth + 1) != 0) {
                free(new_prefix);
                return -1;
            }
        }
    }

    free(new_prefix);
    return 0;
}

/*
 * Build the file-reference path map by starting at the project's
 * mainGroup.  Falls back to each PBXFileReference's own path when no
 * project/mainGroup is present.
 */
static int build_refmap(const struct pbx_node *objects, struct refmap *map)
{
    struct pbx_node *project = NULL;
    const char *main_group = NULL;
    size_t i;

    memset(map, 0, sizeof(*map));

    for (i = 0; i < objects->pair_count; i++) {
        struct pbx_node *obj = objects->pairs[i].value;
        const char *isa;

        if (obj->kind != PBX_DICT) {
            continue;
        }
        isa = pbx_dict_string(obj, "isa");
        if (isa != NULL && strcmp(isa, "PBXProject") == 0) {
            project = obj;
            break;
        }
    }

    if (project != NULL) {
        main_group = pbx_dict_string(project, "mainGroup");
    }

    if (main_group != NULL) {
        if (walk_group(objects, main_group, "", map, 0) != 0) {
            refmap_free(map);
            return -1;
        }
        return 0;
    }

    /* No group tree: record every file reference by its own path. */
    for (i = 0; i < objects->pair_count; i++) {
        struct pbx_node *obj = objects->pairs[i].value;
        const char *isa;
        const char *path;

        if (obj->kind != PBX_DICT) {
            continue;
        }
        isa = pbx_dict_string(obj, "isa");
        if (isa == NULL || strcmp(isa, "PBXFileReference") != 0) {
            continue;
        }
        path = pbx_dict_string(obj, "path");
        if (path == NULL) {
            path = pbx_dict_string(obj, "name");
        }
        if (path == NULL) {
            continue;
        }
        if (refmap_add(map, objects->pairs[i].key, path) != 0) {
            refmap_free(map);
            return -1;
        }
    }
    return 0;
}

/* Append a heap copy of path to a NULL-terminated array. */
static int sources_push(char ***result, size_t *count, size_t *cap,
                        const char *path)
{
    char *copy;

    if (*count + 2 > *cap) {
        size_t next = (*cap == 0) ? 8 : *cap * 2;
        char **grown = realloc(*result, next * sizeof(*grown));

        if (grown == NULL) {
            return -1;
        }
        *result = grown;
        *cap = next;
    }
    copy = strdup(path);
    if (copy == NULL) {
        return -1;
    }
    (*result)[(*count)++] = copy;
    return 0;
}

static int sources_has(char **result, size_t count, const char *path)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (strcmp(result[i], path) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Collect the target's .c sources from its PBXSourcesBuildPhase, in
 * project order, resolving each PBXBuildFile through its fileRef.  The
 * result is a NULL-terminated array the caller frees with free_sources;
 * NULL means the chain was unavailable or empty.
 */
static char **sources_via_build_phase(const struct pbx_node *objects,
                                      const struct pbx_node *target,
                                      const struct refmap *map)
{
    struct pbx_node *phases = pbx_dict_get(target, "buildPhases");
    char **result = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t i;
    size_t j;

    if (phases == NULL || phases->kind != PBX_ARRAY) {
        return NULL;
    }

    for (i = 0; i < phases->item_count; i++) {
        struct pbx_node *phase;
        struct pbx_node *files;

        if (phases->items[i]->kind != PBX_STRING) {
            continue;
        }
        phase = pbx_dict_get(objects, phases->items[i]->string);
        if (phase == NULL || pbx_dict_string(phase, "isa") == NULL ||
            strcmp(pbx_dict_string(phase, "isa"),
                   "PBXSourcesBuildPhase") != 0) {
            continue;
        }

        files = pbx_dict_get(phase, "files");
        if (files == NULL || files->kind != PBX_ARRAY) {
            continue;
        }

        for (j = 0; j < files->item_count; j++) {
            struct pbx_node *build;
            const char *ref_id;
            const char *path;

            if (files->items[j]->kind != PBX_STRING) {
                continue;
            }
            build = pbx_dict_get(objects, files->items[j]->string);
            if (build == NULL) {
                continue;
            }
            ref_id = pbx_dict_string(build, "fileRef");
            if (ref_id == NULL) {
                continue;
            }
            path = refmap_lookup(map, ref_id);
            if (path == NULL) {
                path = ref_id;
            }
            if (!is_c_source(path)) {
                continue;
            }
            if (sources_push(&result, &count, &cap, path) != 0) {
                goto fail;
            }
        }
    }

    if (result == NULL) {
        return NULL;
    }
    result[count] = NULL;
    return result;

fail:
    if (result != NULL) {
        size_t k;
        for (k = 0; k < count; k++) {
            free(result[k]);
        }
        free(result);
    }
    return NULL;
}

/*
 * Fallback for a project with no resolvable PBXSourcesBuildPhase: every
 * resolved file reference whose path names a .c file.
 */
static char **sources_via_file_references(const struct refmap *map)
{
    char **result = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t i;

    for (i = 0; i < map->count; i++) {
        if (!is_c_source(map->paths[i])) {
            continue;
        }
        if (sources_has(result, count, map->paths[i])) {
            continue;
        }
        if (sources_push(&result, &count, &cap, map->paths[i]) != 0) {
            goto fail;
        }
    }

    if (result == NULL) {
        return NULL;
    }
    result[count] = NULL;
    return result;

fail:
    if (result != NULL) {
        size_t k;
        for (k = 0; k < count; k++) {
            free(result[k]);
        }
        free(result);
    }
    return NULL;
}

static void free_sources(char **sources)
{
    size_t i;

    if (sources == NULL) {
        return;
    }
    for (i = 0; sources[i] != NULL; i++) {
        free(sources[i]);
    }
    free(sources);
}

void masa_pbxproj_free(struct masa_pbxproj *proj)
{
    if (proj == NULL) {
        return;
    }
    free(proj->product_name);
    proj->product_name = NULL;
    free_sources(proj->sources);
    proj->sources = NULL;
    proj->source_count = 0;
}

int masa_pbxproj_parse(const char *text, size_t len,
                       struct masa_pbxproj *out, char *err, size_t errcap)
{
    struct pbx_parser ps;
    struct pbx_node *root;
    struct pbx_node *objects;
    struct pbx_node *target = NULL;
    struct refmap refmap;
    const char *name = NULL;
    char **sources = NULL;
    size_t i;
    int rc = -1;

    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    if (text == NULL || len == 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "empty project file");
        }
        return -1;
    }

    ps.p = text;
    ps.end = text + len;
    ps.failed = 0;

    root = parse_value(&ps);
    if (root == NULL || ps.failed) {
        pbx_free(root);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "not a valid property list");
        }
        return -1;
    }

    if (root->kind != PBX_DICT) {
        pbx_free(root);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "top level is not a dictionary");
        }
        return -1;
    }

    objects = objects_dict(root);
    if (objects == NULL || objects->kind != PBX_DICT) {
        pbx_free(root);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "missing objects table");
        }
        return -1;
    }

    if (build_refmap(objects, &refmap) != 0) {
        pbx_free(root);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory resolving file references");
        }
        return -1;
    }

    for (i = 0; i < objects->pair_count; i++) {
        struct pbx_node *obj = objects->pairs[i].value;
        const char *isa;

        if (obj->kind != PBX_DICT) {
            continue;
        }
        isa = pbx_dict_string(obj, "isa");
        if (isa != NULL && strcmp(isa, "PBXNativeTarget") == 0) {
            target = obj;
            break;
        }
    }

    if (target == NULL) {
        refmap_free(&refmap);
        pbx_free(root);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "no PBXNativeTarget in project");
        }
        return -1;
    }

    name = pbx_dict_string(target, "productName");
    if (name == NULL) {
        name = pbx_dict_string(target, "name");
    }
    if (name == NULL || name[0] == '\0') {
        refmap_free(&refmap);
        pbx_free(root);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "PBXNativeTarget has no productName");
        }
        return -1;
    }

    out->product_name = strdup(name);
    if (out->product_name == NULL) {
        refmap_free(&refmap);
        pbx_free(root);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return -1;
    }

    sources = sources_via_build_phase(objects, target, &refmap);
    if (sources == NULL) {
        sources = sources_via_file_references(&refmap);
    }
    refmap_free(&refmap);
    if (sources == NULL) {
        pbx_free(root);
        masa_pbxproj_free(out);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "PBXNativeTarget has no C sources");
        }
        return -1;
    }

    for (i = 0; sources[i] != NULL; i++) {
        /* count */
    }
    out->sources = sources;
    out->source_count = i;
    rc = 0;

    pbx_free(root);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Filesystem helpers                                                 */
/* ------------------------------------------------------------------ */

static int read_whole_file(const char *path, char **out, size_t *out_len,
                           char *err, size_t errcap)
{
    FILE *fp;
    char *buf;
    long size;
    size_t got;

    *out = NULL;
    *out_len = 0;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot open '%s': %s", path,
                     strerror(errno));
        }
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot size '%s': %s", path,
                     strerror(errno));
        }
        fclose(fp);
        return -1;
    }

    buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory reading '%s'", path);
        }
        fclose(fp);
        return -1;
    }

    got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return 0;
}

static int mkdir_p(const char *path)
{
    char tmp[MASA_PATH_MAX];
    char *p;
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, len + 1);

    for (p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void dir_of(const char *path, char *out, size_t outcap)
{
    const char *slash = strrchr(path, '/');

    if (slash == NULL) {
        snprintf(out, outcap, ".");
        return;
    }
    if (slash == path) {
        snprintf(out, outcap, "/");
        return;
    }
    {
        size_t len = (size_t)(slash - path);
        if (len >= outcap) {
            len = outcap - 1;
        }
        memcpy(out, path, len);
        out[len] = '\0';
    }
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return (slash != NULL) ? slash + 1 : path;
}

static int write_text_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");

    if (fp == NULL) {
        return -1;
    }
    if (fputs(text, fp) == EOF || fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* masa xcode build                                                   */
/* ------------------------------------------------------------------ */

/* Strip the SOURCE_ROOT / SRCROOT / ./ prefixes Xcode may record. */
static const char *normalise_source(const char *path)
{
    if (strncmp(path, "SOURCE_ROOT/", 12) == 0) {
        return path + 12;
    }
    if (strncmp(path, "$(SRCROOT)/", 11) == 0) {
        return path + 11;
    }
    if (strncmp(path, "./", 2) == 0) {
        return path + 2;
    }
    return path;
}

/*
 * Compile and link the target's sources into exe by handing them to the
 * Masa cc wrapper, which supplies the target triple, the SDK sysroot and
 * the LLVM linker.
 */
static int compile_and_link(const char *parent,
                            const struct masa_pbxproj *proj, const char *exe)
{
    char *args[MASA_CC_MAX_ARGS];
    char **owned;
    int count = 0;
    size_t i;
    int status;

    owned = calloc(proj->source_count, sizeof(*owned));
    if (owned == NULL) {
        return 1;
    }

    for (i = 0; i < proj->source_count; i++) {
        char joined[MASA_PATH_MAX];
        const char *rel = normalise_source(proj->sources[i]);

        if (count >= MASA_CC_MAX_ARGS - 4) {
            fprintf(stderr, "masa: xcode: too many sources\n");
            goto fail;
        }
        if (snprintf(joined, sizeof(joined), "%s/%s", parent, rel) >=
            (int)sizeof(joined)) {
            fprintf(stderr, "masa: xcode: source path too long: %s\n", rel);
            goto fail;
        }
        owned[i] = strdup(joined);
        if (owned[i] == NULL) {
            goto fail;
        }
        args[count++] = owned[i];
    }

    args[count++] = (char *)"-o";
    args[count++] = (char *)exe;
    args[count] = NULL;

    status = masa_cc_main(count, args);

    for (i = 0; i < proj->source_count; i++) {
        free(owned[i]);
    }
    free(owned);
    return status;

fail:
    for (i = 0; i < proj->source_count; i++) {
        free(owned[i]);
    }
    free(owned);
    return 1;
}

static void info_plist(char *buf, size_t cap, const char *product)
{
    snprintf(buf, cap,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\">\n"
             "<dict>\n"
             "\t<key>CFBundleExecutable</key>\n"
             "\t<string>%s</string>\n"
             "\t<key>CFBundleIdentifier</key>\n"
             "\t<string>com.masa.%s</string>\n"
             "\t<key>CFBundleName</key>\n"
             "\t<string>%s</string>\n"
             "\t<key>CFBundlePackageType</key>\n"
             "\t<string>APPL</string>\n"
             "\t<key>CFBundleShortVersionString</key>\n"
             "\t<string>1.0</string>\n"
             "\t<key>CFBundleVersion</key>\n"
             "\t<string>1</string>\n"
             "</dict>\n"
             "</plist>\n",
             product, product, product);
}

static int xcode_build(int argc, char *const argv[])
{
    const char *project_arg = NULL;
    const char *out_dir = NULL;
    int bundle = 0;
    int i;
    char pbxpath[MASA_PATH_MAX];
    char parent[MASA_PATH_MAX];
    char *text = NULL;
    size_t text_len = 0;
    char err[512];
    struct masa_pbxproj proj;
    char exe[MASA_PATH_MAX];
    char exe_dir[MASA_PATH_MAX];
    struct stat st;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "masa: xcode build: -o needs a directory\n");
                return 2;
            }
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "--bundle") == 0) {
            bundle = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "masa: xcode build: unknown option '%s'\n",
                    argv[i]);
            return 2;
        } else if (project_arg == NULL) {
            project_arg = argv[i];
        } else {
            fprintf(stderr, "masa: xcode build: unexpected argument '%s'\n",
                    argv[i]);
            return 2;
        }
    }

    if (project_arg == NULL) {
        fprintf(stderr,
                "masa: xcode build: missing <path/to/App.xcodeproj>\n");
        return 2;
    }

    if (stat(project_arg, &st) != 0) {
        fprintf(stderr, "masa: xcode build: '%s' does not exist: %s\n",
                project_arg, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr,
                "masa: xcode build: '%s' is not a .xcodeproj directory\n",
                project_arg);
        return 1;
    }

    if (snprintf(pbxpath, sizeof(pbxpath), "%s/%s", project_arg,
                 MASA_PBXPROJ_RELPATH) >= (int)sizeof(pbxpath)) {
        fprintf(stderr, "masa: xcode build: project path too long\n");
        return 1;
    }
    if (stat(pbxpath, &st) != 0) {
        fprintf(stderr, "masa: xcode build: missing %s\n", pbxpath);
        return 1;
    }

    if (read_whole_file(pbxpath, &text, &text_len, err, sizeof(err)) != 0) {
        fprintf(stderr, "masa: xcode build: %s\n", err);
        return 1;
    }

    if (masa_pbxproj_parse(text, text_len, &proj, err, sizeof(err)) != 0) {
        fprintf(stderr, "masa: xcode build: malformed project: %s\n", err);
        free(text);
        return 1;
    }
    free(text);

    dir_of(project_arg, parent, sizeof(parent));
    if (out_dir == NULL) {
        out_dir = parent;
    }

    if (bundle) {
        if (snprintf(exe, sizeof(exe), "%s/%s.app/Contents/MacOS/%s",
                     out_dir, proj.product_name, proj.product_name) >=
            (int)sizeof(exe)) {
            fprintf(stderr, "masa: xcode build: output path too long\n");
            masa_pbxproj_free(&proj);
            return 1;
        }
    } else if (snprintf(exe, sizeof(exe), "%s/%s", out_dir,
                        proj.product_name) >= (int)sizeof(exe)) {
        fprintf(stderr, "masa: xcode build: output path too long\n");
        masa_pbxproj_free(&proj);
        return 1;
    }

    dir_of(exe, exe_dir, sizeof(exe_dir));
    if (mkdir_p(exe_dir) != 0) {
        fprintf(stderr, "masa: xcode build: cannot create '%s': %s\n",
                exe_dir, strerror(errno));
        masa_pbxproj_free(&proj);
        return 1;
    }

    printf("masa: xcode: target '%s' (%zu source%s)\n", proj.product_name,
           proj.source_count, (proj.source_count == 1) ? "" : "s");

    if (compile_and_link(parent, &proj, exe) != 0) {
        fprintf(stderr, "masa: xcode build: compilation failed\n");
        masa_pbxproj_free(&proj);
        return 1;
    }

    if (bundle) {
        char plist[MASA_PATH_MAX];
        char contents[MASA_PATH_MAX];
        char buf[1024];

        if (snprintf(plist, sizeof(plist), "%s/%s.app/Contents/Info.plist",
                     out_dir, proj.product_name) >= (int)sizeof(plist)) {
            masa_pbxproj_free(&proj);
            return 1;
        }
        dir_of(plist, contents, sizeof(contents));
        if (mkdir_p(contents) != 0) {
            fprintf(stderr, "masa: xcode build: cannot create '%s': %s\n",
                    contents, strerror(errno));
            masa_pbxproj_free(&proj);
            return 1;
        }

        info_plist(buf, sizeof(buf), base_name(exe));
        if (write_text_file(plist, buf) != 0) {
            fprintf(stderr, "masa: xcode build: cannot write '%s': %s\n",
                    plist, strerror(errno));
            masa_pbxproj_free(&proj);
            return 1;
        }
        printf("masa: xcode: bundled %s.app\n", proj.product_name);
    } else {
        printf("masa: xcode: built %s\n", exe);
    }

    masa_pbxproj_free(&proj);
    return 0;
}

int masa_xcode_main(int argc, char *const argv[])
{
    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "masa: xcode: missing subcommand (build)\n");
        return 2;
    }
    if (strcmp(argv[0], "build") == 0) {
        return xcode_build(argc - 1, &argv[1]);
    }

    fprintf(stderr, "masa: xcode: unknown subcommand '%s'\n", argv[0]);
    return 2;
}
