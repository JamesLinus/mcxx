/*!if GRAMMAR_PROLOGUE */

%token<token_atrib> TOKEN_DECLSPEC "__declspec"

%token<token_atrib> MS_INT8 "__int8"
%token<token_atrib> MS_INT16 "__int16"
%token<token_atrib> MS_INT32 "__int32"
%token<token_atrib> MS_INT64 "__int64"

%type<ast> declspec_specifier
%type<ast> extended_decl_modifier_list
%type<ast> extended_decl_modifier_list0
%type<ast> extended_decl_modifier

/*!endif*/
/*!if GRAMMAR_RULES*/
nontype_specifier : declspec_specifier
{
    $$ = $1;
}
;

declspec_specifier : TOKEN_DECLSPEC '(' extended_decl_modifier_list ')'
{
    $$ = ASTMake1(AST_MS_DECLSPEC, $3, make_locus($1.token_file, $1.token_line, 0), $1.token_text);
}
;

extended_decl_modifier_list : /* empty */
{
    $$ = NULL;
}
| extended_decl_modifier_list0
{
    $$ = $1;
}
;

extended_decl_modifier_list0 : extended_decl_modifier
{
    $$ = ASTListLeaf($1);
}
| extended_decl_modifier_list0 ',' extended_decl_modifier
{
    $$ = ASTList($1, $3);
}
;

extended_decl_modifier : IDENTIFIER
{
    $$ = ASTMake1(AST_MS_DECLSPEC_ITEM, NULL, make_locus($1.token_file, $1.token_line, 0), $1.token_text);
}
| IDENTIFIER '(' expression_list ')'
{
    $$ = ASTMake1(AST_MS_DECLSPEC_ITEM, $3, make_locus($1.token_file, $1.token_line, 0), $1.token_text);
}
;

attribute_specifier : declspec_specifier
{
    $$ = $1;
}
;

builtin_types : MS_INT8
{
    $$ = ASTLeaf(AST_MS_INT8, make_locus($1.token_file, $1.token_line, 0), NULL);
}
| MS_INT16
{
    $$ = ASTLeaf(AST_MS_INT16, make_locus($1.token_file, $1.token_line, 0), NULL);
}
| MS_INT32
{
    $$ = ASTLeaf(AST_MS_INT32, make_locus($1.token_file, $1.token_line, 0), NULL);
}
| MS_INT64
{
    $$ = ASTLeaf(AST_MS_INT64, make_locus($1.token_file, $1.token_line, 0), NULL);
}
;

/*!endif*/
