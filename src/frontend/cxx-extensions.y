/*!if GRAMMAR_PROLOGUE*/

%type<ast> shape_seq
%type<ast> shape
%type<ast> noshape_cast_expression

/*!endif*/
/*!if GRAMMAR_RULES*/

postfix_expression : postfix_expression '[' expression_opt ':' expression_opt ']'
{
    $$ = ASTMake4(AST_ARRAY_SECTION, $1, $3, $5, NULL, ast_get_locus($1), NULL);
}
| postfix_expression '[' expression_opt ':' expression_opt ':' expression ']'
{
    $$ = ASTMake4(AST_ARRAY_SECTION, $1, $3, $5, $7, ast_get_locus($1), NULL);
}
| postfix_expression '[' expression ';' expression ']'
{
    $$ = ASTMake4(AST_ARRAY_SECTION_SIZE, $1, $3, $5, NULL, ast_get_locus($1), NULL);
}
| postfix_expression '[' expression ';' expression ':' expression ']'
{
    $$ = ASTMake4(AST_ARRAY_SECTION_SIZE, $1, $3, $5, $7, ast_get_locus($1), NULL);
}
;

noshape_cast_expression : unary_expression %merge<ambiguityHandler>
{
	$$ = $1;
}
| '(' type_id ')' cast_expression %merge<ambiguityHandler>
{
	$$ = ASTMake2(AST_CAST, $2, $4, make_locus($1.token_file, $1.token_line, 0), NULL);
}
;

cast_expression : shape_seq noshape_cast_expression %merge<ambiguityHandler>
{
    $$ = ASTMake2(AST_SHAPING_EXPRESSION, $1, $2, ast_get_locus($1), NULL);
}
;

shape_seq : shape_seq shape %dprec 2
{
    $$ = ASTList($1, $2);
}
| shape %dprec 1
{
    $$ = ASTListLeaf($1);
}
;

shape: '[' expression ']'
{
    $$ = $2;
}
;

iteration_statement : FOR '[' symbol_literal_ref ']' '(' for_init_statement condition_opt ';' expression_opt ')' statement
{
    AST loop_control = ASTMake3(AST_LOOP_CONTROL, $6, $7, $9, make_locus($1.token_file, $1.token_line, 0), NULL);
	$$ = ASTMake4(AST_FOR_STATEMENT, loop_control, $11, NULL, $3, make_locus($1.token_file, $1.token_line, 0), NULL);

}
;

/*!endif*/
