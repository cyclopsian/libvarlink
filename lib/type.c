#include "error.h"
#include "interface.h"
#include "scanner.h"
#include "service.h"
#include "util.h"

#include <ctype.h>
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

long varlink_type_new(VarlinkType **typep, const char *typestring) {
        _cleanup_(varlink_type_unrefp) VarlinkType *type = NULL;
        _cleanup_(scanner_freep) Scanner *scanner = NULL;

        scanner_new_interface(&scanner, typestring);

        if (!varlink_type_new_from_scanner(&type, scanner) ||
            scanner_peek(scanner) != '\0')
                return -VARLINK_ERROR_INVALID_INTERFACE;

        *typep = type;
        type = NULL;

        return 0;
}

bool varlink_type_new_from_scanner(VarlinkType **typep, Scanner *scanner) {
        _cleanup_(varlink_type_unrefp) VarlinkType *type = NULL;

        if (scanner_read_keyword(scanner, "bool"))
                varlink_type_allocate(&type, VARLINK_TYPE_BOOL);

        else if (scanner_read_keyword(scanner, "int"))
                varlink_type_allocate(&type, VARLINK_TYPE_INT);

        else if (scanner_read_keyword(scanner, "float"))
                varlink_type_allocate(&type, VARLINK_TYPE_FLOAT);

        else if (scanner_read_keyword(scanner, "string"))
                varlink_type_allocate(&type, VARLINK_TYPE_STRING);

        else if (scanner_read_keyword(scanner, "object"))
                varlink_type_allocate(&type, VARLINK_TYPE_FOREIGN_OBJECT);

        else if (scanner_peek(scanner) ==  '(') {
                unsigned long n_fields_allocated = 0;

                scanner_expect_operator(scanner, "(");
                varlink_type_allocate(&type, VARLINK_TYPE_OBJECT);

                for (unsigned long i = 0; scanner_peek(scanner) != ')'; i += 1) {
                        _cleanup_(varlink_type_field_freep) VarlinkTypeField *field = NULL;

                        if (i > 0 && !scanner_expect_operator(scanner, ","))
                                return false;

                        field = calloc(1, sizeof(VarlinkTypeField));
                        field->description = scanner_get_last_docstring(scanner);

                        if (!scanner_expect_field_name(scanner, &field->name))
                                return false;

                        if (i == 0 && scanner_peek(scanner) != ':')
                                type->kind = VARLINK_TYPE_ENUM;

                        if (type->kind == VARLINK_TYPE_OBJECT) {
                                if (!scanner_expect_operator(scanner, ":") ||
                                    !varlink_type_new_from_scanner(&field->type, scanner))
                                        return false;
                        }

                        /* make sure a field with this name doesn't exist yet */
                        if (avl_tree_insert(type->fields_sorted, field->name, field) < 0)
                                return scanner_error(scanner, SCANNER_ERROR_DUPLICATE_FIELD_NAME, field->name);

                        if (type->n_fields == n_fields_allocated) {
                                n_fields_allocated = MAX(n_fields_allocated * 2, 4);
                                type->fields = realloc(type->fields, n_fields_allocated * sizeof(VarlinkType *));
                        }

                        type->fields[i] = field;
                        type->n_fields += 1;
                        field = NULL;
                }

                if (!scanner_expect_operator(scanner, ")"))
                        return false;

        } else {
                char *alias;

                if (!scanner_expect_type_name(scanner, &alias))
                        return scanner_error(scanner, SCANNER_ERROR_TYPE_EXPECTED, NULL);

                varlink_type_allocate(&type, VARLINK_TYPE_ALIAS);
                type->alias = alias;
        }

        if (scanner_peek(scanner) == '[') {
                _cleanup_(varlink_type_unrefp) VarlinkType *array = NULL;

                varlink_type_allocate(&array, VARLINK_TYPE_ARRAY);

                scanner_expect_operator(scanner, "[");
                if ((isdigit(scanner_peek(scanner)) && !scanner_read_uint(scanner, &array->fixed_n_elements)) ||
                    !scanner_expect_operator(scanner, "]"))
                        return false;

                array->element_type = type;
                type = array;
                array = NULL;
        }

        *typep = type;
        type = NULL;

        return true;
}

static VarlinkTypeField *varlink_type_field_free(VarlinkTypeField *field) {
        if (field->name)
                free(field->name);

        if (field->type)
                varlink_type_unref(field->type);

        free(field->description);
        free(field);

        return NULL;
}

void varlink_type_field_freep(VarlinkTypeField **fieldp) {
        if (*fieldp)
                varlink_type_field_free(*fieldp);
}

VarlinkType *varlink_type_field_get_type(VarlinkType *type, const char *name) {
        VarlinkTypeField *field;

        assert(type->kind == VARLINK_TYPE_OBJECT);

        field = avl_tree_find(type->fields_sorted, name);
        if (!field)
                return NULL;

        return field->type;
}

static long field_compare(const void *key, void *value) {
        VarlinkTypeField *field = value;

        return strcmp(key, field->name);
}

long varlink_type_allocate(VarlinkType **typep,
                           VarlinkTypeKind kind) {
        VarlinkType *type;

        type = calloc(1, sizeof(VarlinkType));
        type->refcount = 1;
        type->kind = kind;

        if (kind == VARLINK_TYPE_OBJECT)
                avl_tree_new(&type->fields_sorted, field_compare, NULL);

        *typep = type;

        return 0;
}

VarlinkType *varlink_type_ref(VarlinkType *type) {
        type->refcount += 1;

        return type;
}

VarlinkType *varlink_type_unref(VarlinkType *type) {
        assert(type->refcount > 0);

        type->refcount -= 1;

        if (type->refcount == 0) {
                switch (type->kind) {
                        case VARLINK_TYPE_BOOL:
                        case VARLINK_TYPE_INT:
                        case VARLINK_TYPE_FLOAT:
                        case VARLINK_TYPE_STRING:
                        case VARLINK_TYPE_FOREIGN_OBJECT:
                                break;

                        case VARLINK_TYPE_ENUM:
                        case VARLINK_TYPE_OBJECT:
                                for (unsigned long i = 0; i < type->n_fields; i += 1)
                                        varlink_type_field_free(type->fields[i]);

                                free(type->fields);
                                avl_tree_free(type->fields_sorted);
                                break;

                        case VARLINK_TYPE_ARRAY:
                                if (type->element_type)
                                        varlink_type_unref(type->element_type);
                                break;

                        case VARLINK_TYPE_ALIAS:
                                free(type->alias);
                                break;
                }

                free(type->typestring);
                free(type);
        }

        return NULL;
}

void varlink_type_unrefp(VarlinkType **typep) {
        if (*typep)
                varlink_type_unref(*typep);
}

static bool is_multiline(VarlinkType *type) {
        /* "()" */
        if (type->n_fields == 0)
                return false;

        /* A maximum of two object fields */
        if (type->kind == VARLINK_TYPE_OBJECT && type->n_fields > 2)
                return true;

        for (unsigned long i = 0; i < type->n_fields; i += 1) {
                VarlinkTypeField *field = type->fields[i];

                /* No documentation */
                if (field->description)
                        return true;

                /* No nested complex types */
                if (type->kind == VARLINK_TYPE_OBJECT) {
                        if (field->type->kind == VARLINK_TYPE_OBJECT ||
                            field->type->kind == VARLINK_TYPE_ENUM)
                                return true;
                }
        }

        /* No longer than half a line */
        if (strlen(varlink_type_get_typestring(type)) > 40)
                return true;

        return false;
}

static void varlink_type_print(VarlinkType *type,
                               FILE *stream,
                               long indent,
                               const char *comment_pre, const char *comment_post,
                               const char *type_pre, const char *type_post) {
        if (!type_pre)
                type_pre = "";

        if (!type_post)
                type_post = "";

        switch (type->kind) {
                case VARLINK_TYPE_BOOL:
                        fprintf(stream, "%sbool%s", type_pre, type_post);
                        break;

                case VARLINK_TYPE_INT:
                        fprintf(stream, "%sint%s", type_pre, type_post);
                        break;

                case VARLINK_TYPE_FLOAT:
                        fprintf(stream, "%sfloat%s", type_pre, type_post);
                        break;

                case VARLINK_TYPE_STRING:
                        fprintf(stream, "%sstring%s", type_pre, type_post);
                        break;

                case VARLINK_TYPE_ENUM:
                case VARLINK_TYPE_OBJECT: {
                        bool multiline = false;
                        bool docstring_printed = false;

                        if (indent >= 0)
                                multiline = is_multiline(type);

                        fprintf(stream, "(");

                        for (unsigned long i = 0; i < type->n_fields; i += 1) {
                                VarlinkTypeField *field = type->fields[i];

                                if (multiline) {
                                        fprintf(stream, "\n");

                                        if (field->description) {
                                                if (i > 0 && !docstring_printed)
                                                        fprintf(stream, "\n");

                                                for (const char *start = field->description; *start;) {
                                                        const char *end = strchrnul(start, '\n');
                                                        int len = end - start;

                                                        for (long l = 0; l < indent + 1; l += 1)
                                                                fprintf(stream, "  ");

                                                        fprintf(stream, "%s#", comment_pre);
                                                        if (len > 0)
                                                                fprintf(stream, " %.*s", len, start);
                                                        fprintf(stream, "%s\n", comment_post);

                                                        if (*end != '\n')
                                                                break;

                                                        start = end + 1;
                                                }

                                                docstring_printed = true;
                                        } else
                                                docstring_printed = false;

                                        for (long l = 0; l < indent + 1; l += 1)
                                                fprintf(stream, "  ");
                                }

                                fprintf(stream, "%s", field->name);

                                if (type->kind == VARLINK_TYPE_OBJECT) {
                                        fprintf(stream, ": ");

                                        varlink_type_print(field->type,
                                                           stream,
                                                           indent >= 0 ? indent + 1 : -1,
                                                           comment_pre, comment_post,
                                                           type_pre, type_post);
                                }

                                if (i + 1 < type->n_fields) {
                                        fprintf(stream, ",");

                                        if (!multiline)
                                                fprintf(stream, " ");

                                        if (multiline && field->description)
                                                fprintf(stream, "\n");
                                }
                        }

                        if (multiline) {
                                fprintf(stream, "\n");

                                for (long l = 0; l < indent; l += 1)
                                        fprintf(stream, "  ");
                        }

                        fprintf(stream, ")");
                        break;
                }

                case VARLINK_TYPE_ARRAY:
                        varlink_type_print(type->element_type,
                                           stream,
                                           indent,
                                           comment_pre, comment_post,
                                           type_pre, type_post);
                        fprintf(stream, "[");

                        if (type->fixed_n_elements > 0)
                                fprintf(stream, "%lu", type->fixed_n_elements);

                        fprintf(stream, "]");
                        break;

                case VARLINK_TYPE_FOREIGN_OBJECT:
                        fprintf(stream, "%sobject%s", type_pre, type_post);
                        break;

                case VARLINK_TYPE_ALIAS:
                        fprintf(stream, "%s%s%s", type_pre, type->alias, type_post);
                        break;

                default:
                        abort();
        }
}

const char *varlink_type_get_typestring(VarlinkType *type) {
        FILE *stream = NULL;
        _cleanup_(freep) char *string = NULL;
        unsigned long size;

        if (type->typestring)
                return type->typestring;

        stream = open_memstream(&type->typestring, &size);
        varlink_type_print(type, stream, -1, NULL, NULL, NULL, NULL);

        fclose(stream);

        return type->typestring;
}

long varlink_type_write_typestring(VarlinkType *type,
                                   FILE *stream,
                                   long indent,
                                   const char *comment_pre, const char *comment_post,
                                   const char *type_pre, const char *type_post) {
        if (!type_pre)
                type_pre = "";

        if (!type_post)
                type_post = "";

        varlink_type_print(type,
                           stream,
                           indent,
                           comment_pre, comment_post,
                           type_pre, type_post);

        return 0;
}
