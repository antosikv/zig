/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of zig, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "config.h"
#include "buffer.hpp"
#include "codegen.hpp"
#include "os.hpp"
#include "error.hpp"
#include "parseh.hpp"

#include <stdio.h>

static int usage(const char *arg0) {
    fprintf(stderr, "Usage: %s [command] [options]\n"
        "Commands:\n"
        "  build                  create executable, object, or library from target\n"
        "  version                print version number and exit\n"
        "  parseh                 convert a c header file to zig extern declarations\n"
        "Options:\n"
        "  --release              build with optimizations on and debug protection off\n"
        "  --static               output will be statically linked\n"
        "  --strip                exclude debug symbols\n"
        "  --export [exe|lib|obj] override output type\n"
        "  --name [name]          override output name\n"
        "  --output [file]        override destination path\n"
        "  --verbose              turn on compiler debug output\n"
        "  --color [auto|off|on]  enable or disable colored error messages\n"
        "  --libc-path [path]     set the C compiler data path\n"
        "  -isystem [dir]         add additional search path for other .h files\n"
        "  -dirafter [dir]        same as -isystem but do it last\n"
    , arg0);
    return EXIT_FAILURE;
}

static int version(const char *arg0, int argc, char **argv) {
    printf("%s\n", ZIG_VERSION_STRING);
    return EXIT_SUCCESS;
}

struct Build {
    const char *in_file;
    const char *out_file;
    bool release;
    bool strip;
    bool is_static;
    OutType out_type;
    const char *out_name;
    bool verbose;
    ErrColor color;
    const char *libc_path;
    ZigList<const char *> clang_argv;
};

static int build(const char *arg0, int argc, char **argv) {
    int err;
    Build b = {0};

    for (int i = 0; i < argc; i += 1) {
        char *arg = argv[i];
        if (arg[0] == '-') {
            if (strcmp(arg, "--release") == 0) {
                b.release = true;
            } else if (strcmp(arg, "--strip") == 0) {
                b.strip = true;
            } else if (strcmp(arg, "--static") == 0) {
                b.is_static = true;
            } else if (strcmp(arg, "--verbose") == 0) {
                b.verbose = true;
            } else if (i + 1 >= argc) {
                return usage(arg0);
            } else {
                i += 1;
                if (i >= argc) {
                    return usage(arg0);
                } else if (strcmp(arg, "--output") == 0) {
                    b.out_file = argv[i];
                } else if (strcmp(arg, "--export") == 0) {
                    if (strcmp(argv[i], "exe") == 0) {
                        b.out_type = OutTypeExe;
                    } else if (strcmp(argv[i], "lib") == 0) {
                        b.out_type = OutTypeLib;
                    } else if (strcmp(argv[i], "obj") == 0) {
                        b.out_type = OutTypeObj;
                    } else {
                        return usage(arg0);
                    }
                } else if (strcmp(arg, "--color") == 0) {
                    if (strcmp(argv[i], "auto") == 0) {
                        b.color = ErrColorAuto;
                    } else if (strcmp(argv[i], "on") == 0) {
                        b.color = ErrColorOn;
                    } else if (strcmp(argv[i], "off") == 0) {
                        b.color = ErrColorOff;
                    } else {
                        return usage(arg0);
                    }
                } else if (strcmp(arg, "--name") == 0) {
                    b.out_name = argv[i];
                } else if (strcmp(arg, "--libc-path") == 0) {
                    b.libc_path = argv[i];
                } else if (strcmp(arg, "-isystem") == 0) {
                    b.clang_argv.append("-isystem");
                    b.clang_argv.append(argv[i]);
                } else if (strcmp(arg, "-dirafter") == 0) {
                    b.clang_argv.append("-dirafter");
                    b.clang_argv.append(argv[i]);
                } else {
                    return usage(arg0);
                }
            }
        } else if (!b.in_file) {
            b.in_file = arg;
        } else {
            return usage(arg0);
        }
    }

    if (!b.in_file)
        return usage(arg0);

    Buf in_file_buf = BUF_INIT;
    buf_init_from_str(&in_file_buf, b.in_file);

    Buf root_source_dir = BUF_INIT;
    Buf root_source_code = BUF_INIT;
    Buf root_source_name = BUF_INIT;
    if (buf_eql_str(&in_file_buf, "-")) {
        os_get_cwd(&root_source_dir);
        if ((err = os_fetch_file(stdin, &root_source_code))) {
            fprintf(stderr, "unable to read stdin: %s\n", err_str(err));
            return 1;
        }
        buf_init_from_str(&root_source_name, "");
    } else {
        os_path_split(&in_file_buf, &root_source_dir, &root_source_name);
        if ((err = os_fetch_file_path(buf_create_from_str(b.in_file), &root_source_code))) {
            fprintf(stderr, "unable to open '%s': %s\n", b.in_file, err_str(err));
            return 1;
        }
    }

    CodeGen *g = codegen_create(&root_source_dir);
    codegen_set_build_type(g, b.release ? CodeGenBuildTypeRelease : CodeGenBuildTypeDebug);
    codegen_set_clang_argv(g, b.clang_argv.items, b.clang_argv.length);
    codegen_set_strip(g, b.strip);
    codegen_set_is_static(g, b.is_static);
    if (b.out_type != OutTypeUnknown)
        codegen_set_out_type(g, b.out_type);
    if (b.out_name)
        codegen_set_out_name(g, buf_create_from_str(b.out_name));
    if (b.libc_path)
        codegen_set_libc_path(g, buf_create_from_str(b.libc_path));
    codegen_set_verbose(g, b.verbose);
    codegen_set_errmsg_color(g, b.color);
    codegen_add_root_code(g, &root_source_dir, &root_source_name, &root_source_code);
    codegen_link(g, b.out_file);

    return 0;
}

struct ParseHPrint {
    ParseH parse_h;
    FILE *f;
    int cur_indent;
};

static const int indent_size = 4;

static void print_indent(ParseHPrint *p) {
    for (int i = 0; i < p->cur_indent; i += 1) {
        fprintf(p->f, " ");
    }
}

static Buf *node_to_buf(AstNode *node) {
    if (node->type == NodeTypeSymbol) {
        return &node->data.symbol_expr.symbol;
    } else if (node->type == NodeTypePrefixOpExpr) {
        PrefixOp op = node->data.prefix_op_expr.prefix_op;
        const char *child_type_str = buf_ptr(node_to_buf(node->data.prefix_op_expr.primary_expr));
        if (op == PrefixOpAddressOf) {
            return buf_sprintf("&%s", child_type_str);
        } else if (op == PrefixOpConstAddressOf) {
            return buf_sprintf("&const %s", child_type_str);
        } else {
            zig_unreachable();
        }
    } else {
        zig_unreachable();
    }
}

static int parseh(const char *arg0, int argc, char **argv) {
    char *in_file = nullptr;
    ZigList<const char *> clang_argv = {0};
    for (int i = 0; i < argc; i += 1) {
        char *arg = argv[i];
        if (arg[0] == '-') {
            if (arg[1] == 'I') {
                clang_argv.append(arg);
            } else if (strcmp(arg, "-isystem") == 0) {
                if (i + 1 >= argc) {
                    return usage(arg0);
                }
                clang_argv.append("-isystem");
                clang_argv.append(argv[i + 1]);
                i += 1;
            } else {
                fprintf(stderr, "unrecognized argument: %s", arg);
                return usage(arg0);
            }
        } else if (!in_file) {
            in_file = arg;
        } else {
            return usage(arg0);
        }
    }
    if (!in_file) {
        fprintf(stderr, "missing target argument");
        return usage(arg0);
    }

    clang_argv.append(in_file);

    Buf *libc_include_path = buf_alloc();
    os_path_join(buf_create_from_str(ZIG_LIBC_DIR), buf_create_from_str("include"), libc_include_path);
    clang_argv.append("-isystem");
    clang_argv.append(buf_ptr(libc_include_path));

    ParseHPrint parse_h_print = {{{0}}};
    ParseHPrint *p = &parse_h_print;
    p->f = stdout;
    p->cur_indent = 0;

    parse_h_file(&p->parse_h, &clang_argv);

    if (p->parse_h.errors.length > 0) {
        for (int i = 0; i < p->parse_h.errors.length; i += 1) {
            ErrorMsg *err_msg = p->parse_h.errors.at(i);
            // TODO respect --color arg
            print_err_msg(err_msg, ErrColorAuto);
        }
        return EXIT_FAILURE;
    }

    for (int var_i = 0; var_i < p->parse_h.var_list.length; var_i += 1) {
        AstNode *var_decl = p->parse_h.var_list.at(var_i);
        assert(var_decl->type == NodeTypeVariableDeclaration);
        const char *pub_str = (var_decl->data.variable_declaration.visib_mod == VisibModPub) ? "pub " : "";
        const char *extern_str = var_decl->data.variable_declaration.is_extern ? "extern " : "";
        const char *var_name = buf_ptr(&var_decl->data.variable_declaration.symbol);
        const char *const_or_var = var_decl->data.variable_declaration.is_const ? "const" : "var";
        print_indent(p);
        fprintf(p->f, "%s%s%s %s", pub_str, extern_str, const_or_var, var_name);
        if (var_decl->data.variable_declaration.type) {
            fprintf(p->f, ": %s", buf_ptr(node_to_buf(var_decl->data.variable_declaration.type)));
        }
        if (var_decl->data.variable_declaration.expr) {
            fprintf(p->f, " = %s", buf_ptr(node_to_buf(var_decl->data.variable_declaration.expr)));
        }
        fprintf(p->f, ";\n");
    }

    for (int struct_i = 0; struct_i < p->parse_h.struct_list.length; struct_i += 1) {
        AstNode *struct_decl = p->parse_h.struct_list.at(struct_i);
        assert(struct_decl->type == NodeTypeStructDecl);
        const char *struct_name = buf_ptr(&struct_decl->data.struct_decl.name);
        print_indent(p);
        fprintf(p->f, "struct %s {\n", struct_name);
        p->cur_indent += indent_size;
        for (int field_i = 0; field_i < struct_decl->data.struct_decl.fields.length; field_i += 1) {
            AstNode *field_node = struct_decl->data.struct_decl.fields.at(field_i);
            assert(field_node->type == NodeTypeStructField);
            const char *field_name = buf_ptr(&field_node->data.struct_field.name);
            Buf *type_name = node_to_buf(field_node->data.struct_field.type);
            print_indent(p);
            fprintf(p->f, "%s: %s,\n", field_name, buf_ptr(type_name));
        }

        p->cur_indent -= indent_size;
        fprintf(p->f, "}\n\n");
    }

    for (int fn_i = 0; fn_i < p->parse_h.fn_list.length; fn_i += 1) {
        AstNode *fn_proto = p->parse_h.fn_list.at(fn_i);
        assert(fn_proto->type == NodeTypeFnProto);
        print_indent(p);
        const char *fn_name = buf_ptr(&fn_proto->data.fn_proto.name);
        const char *pub_str = (fn_proto->data.fn_proto.visib_mod == VisibModPub) ? "pub " : "";
        const char *extern_str = fn_proto->data.fn_proto.is_extern ? "extern " : "";
        fprintf(p->f, "%s%sfn %s(", pub_str, extern_str, fn_name);
        int arg_count = fn_proto->data.fn_proto.params.length;
        bool is_var_args = fn_proto->data.fn_proto.is_var_args;
        for (int arg_i = 0; arg_i < arg_count; arg_i += 1) {
            AstNode *param_decl = fn_proto->data.fn_proto.params.at(arg_i);
            assert(param_decl->type == NodeTypeParamDecl);
            const char *arg_name = buf_ptr(&param_decl->data.param_decl.name);
            Buf *arg_type = node_to_buf(param_decl->data.param_decl.type);
            const char *noalias_str = param_decl->data.param_decl.is_noalias ? "noalias " : "";
            fprintf(p->f, "%s%s: %s", noalias_str, arg_name, buf_ptr(arg_type));
            if (arg_i + 1 < arg_count || is_var_args) {
                fprintf(p->f, ", ");
            }
        }
        if (is_var_args) {
            fprintf(p->f, "...");
        }
        fprintf(p->f, ")");
        Buf *return_type_name = node_to_buf(fn_proto->data.fn_proto.return_type);
        if (!buf_eql_str(return_type_name, "void")) {
            fprintf(p->f, " -> %s", buf_ptr(return_type_name));
        }
        fprintf(p->f, ";\n");
    }

    return 0;
}

int main(int argc, char **argv) {
    char *arg0 = argv[0];
    int (*cmd)(const char *, int, char **) = nullptr;
    for (int i = 1; i < argc; i += 1) {
        char *arg = argv[i];
        if (arg[0] == '-' && arg[1] == '-') {
            return usage(arg0);
        } else {
            if (strcmp(arg, "build") == 0) {
                cmd = build;
            } else if (strcmp(arg, "version") == 0) {
                cmd = version;
            } else if (strcmp(arg, "parseh") == 0) {
                cmd = parseh;
            } else {
                fprintf(stderr, "Unrecognized command: %s\n", arg);
                return usage(arg0);
            }
            return cmd(arg0, argc - i - 1, &argv[i + 1]);
        }
    }

    return usage(arg0);
}
