/*
    Mercurium C/C++ Compiler
    Copyright (C) 2006-2008 - Roger Ferrer Ibanez <roger.ferrer@bsc.es>
    Barcelona Supercomputing Center - Centro Nacional de Supercomputacion
    Universitat Politecnica de Catalunya

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifndef CXX_AST_H
#define CXX_AST_H

/*
 * Abstract syntax tree
 */

#include "extstruct.h"
#include "cxx-macros.h"
#include "cxx-ast-decls.h"
#include "cxx-asttype.h"
#include "cxx-type-decls.h"
#include "cxx-scopelink-decls.h"

MCXX_BEGIN_DECLS

#define MAX_AST_CHILDREN (4)

// The extensible schema of AST's
extern extensible_schema_t ast_extensible_schema;

// Returns the type of the node
node_t ast_get_type(const_AST a);

// Returns the parent node or NULL if none
AST ast_get_parent(const_AST a);

// Sets the parent (but does not update the parent
// to point 'a')
void ast_set_parent(AST a, AST parent);

// Returns the line of the node
int ast_get_line(const_AST a);

// Returns the related bit of text of the node
const char* ast_get_text(const_AST a);

// Sets the related text
void ast_set_text(AST a, const char* str);

// Returns the children 'num_child'. Might be
// NULL
AST ast_get_child(const_AST a, int num_child);

// Sets the children, this one is preferred over ASTSon{0,1,2,3}
// Note that this sets the parent of new_children
void ast_set_child(AST a, int num_child, AST new_children);

// Main routine to create a node
AST ast_make(node_t type, int num_children, 
        AST son0, 
        AST son1, 
        AST son2, 
        AST son3, 
        int line, 
        const char *text);

// Returns the number of children as defined
// by ASTMake{1,2,3} or ASTLeaf
int ast_num_children(const_AST a);

// Returns the filename
const char *ast_get_filename(const_AST a);

// Sets the filename (seldomly needed)
void ast_set_filename(AST a, const char* str);

// A list leaf (a special kind of node used to store lists in reverse order as
// generated by the LR parser)
//
// This is a leaf (so, a list with only one element)
AST ast_list_leaf(AST elem);

// Creates a tree where last_element has been appended onto previous_list
AST ast_list(AST previous_list, AST last_element);

// Returns the type we tagged this tree, NULL if the
// tree was not tagged
struct type_tag* ast_get_expression_type(const_AST a);
// Sets the value of the type expression
void ast_set_expression_type(AST a, struct type_tag*);
// Do not use this one, it is here to implement ASTExprType below
struct type_tag** ast_expression_type_ref(AST a);

// States if the expression is a Lvalue, only meaningful
// if ast_expression_type returned something non NULL
char ast_get_expression_is_lvalue(const_AST a);
// Sets the lvalueness of an expression
void ast_set_expression_is_lvalue(AST a, char c);
// Do not use this one, it is here to implement ASTExprLvalue below
char *ast_expression_is_lvalue_ref(AST a);

// Returns the extensible struct of this AST
extensible_struct_t* ast_get_extensible_struct(const_AST a);

// States if this portion of the tree is properly linked
char ast_check(const_AST a);

// Gives a copy of all the tree
AST ast_copy(const_AST a);

// Gives a copy of all the tree but removing dependent types
AST ast_copy_for_instantiation(const_AST a);

// This makes a bitwise copy. You must know what you
// are doing here! *dest = *src
void ast_replace(AST dest, const_AST src);

// Returns a newly allocated string with a pair 'filename:line'
// of the node
char* ast_location(const_AST a);

// Returns a printed version of the node name
const char* ast_node_type_name(node_t n);

// States if two trees are the same
char ast_equal (const_AST ast1, const_AST ast2);

// It only checks that the two nodes contain
// the same basic information
char ast_equal_node (const_AST ast1, const_AST ast2);

// Returns the number (as it would be used in ast_get_child or ast_set_child)
// of the child 'child'. Returns -1 if 'child' is not a child of 'a'
int ast_num_of_given_child(const_AST a, const_AST child);

// Synthesizes an ambiguous node given two nodes (possibly ambiguous too)
AST ast_make_ambiguous(AST son0, AST son1);

// Returns the number of ambiguities
int ast_get_num_ambiguities(const_AST a);

// Returns the ambiguity 'num'
AST ast_get_ambiguity(const_AST a, int num);

// Replace a node with the ambiguity 'num'. This is
// used when solving ambiguities
void ast_replace_with_ambiguity(AST a, int num);

// ScopeLink function
AST ast_copy_with_scope_link(AST a, scope_link_t* orig, scope_link_t* new_sl);

/*
 * Macros
 *
 * Most of them are here for compatibility purposes. New code
 * should prefer the functions described above (except for those
 * otherwise stated)
 */

#define ASTType(a) ast_get_type(a)
#define ASTParent(a) (ast_get_parent(a))
// ASTLine hardened to avoid problems (not a lvalue)
#define ASTLine(a) ast_get_line(a)
// #define ASTLineLval(a) ((a)->line)
#define ASTText(a) ast_get_text(a)
#define ASTSon0(a) (ast_get_child(a, 0))
#define ASTSon1(a) (ast_get_child(a, 1))
#define ASTSon2(a) (ast_get_child(a, 2))
#define ASTSon3(a) (ast_get_child(a, 3))

#define ASTLeaf(node, line, text) ast_make(node, 0,  NULL, NULL, NULL, NULL, line, text)
#define ASTMake1(node, son0, line, text) ast_make(node, 1, son0, NULL, NULL, NULL, line, text)
#define ASTMake2(node, son0, son1, line, text) ast_make(node, 2, son0, son1, NULL, NULL, line, text)
#define ASTMake3(node, son0, son1, son2, line, text) ast_make(node, 3, son0, son1, son2, NULL, line, text)
#define ASTMake4(node, son0, son1, son2, son3, line, text) ast_make(node, 4, son0, son1, son2, son3, line, text)

// Convenience macros
#define ASTChild0(a) ASTChild(a, 0)
#define ASTChild1(a) ASTChild(a, 1)
#define ASTChild2(a) ASTChild(a, 2)
#define ASTChild3(a) ASTChild(a, 3)
#define ASTChild(a, n) (ast_get_child(a, n))

#define ASTNumChildren(a) (ast_num_children(a))

#define ASTFileName(a) (ast_get_filename(a))

#define ASTListLeaf(a) ast_list_leaf(a)
#define ASTList(list,element) ast_list(list,element)

// Expression type
#define ASTExprType(a) (ast_get_expression_type(a))
#define ASTExprLvalue(a) (ast_get_expression_is_lvalue(a))

// Extensible structure function
#define ASTAttrValue(_a, _name) \
    ( \
      extensible_struct_get_field_pointer(&ast_extensible_schema, ast_get_extensible_struct(_a), (_name)) \
    )

#define ASTAttrValueType(_a, _name, _type) \
    ( (*(_type*)(ASTAttrValue((_a), (_name)))))

#define ASTAttrSetValueType(_a, _name, _type, _value) \
    ( ASTAttrValueType((_a), (_name), _type) = _value )


#define ASTCheck ast_check

#define ast_duplicate ast_copy
#define ast_duplicate_for_instantiation ast_copy_for_instantiation

#define ast_print_node_type ast_node_type_name

#define get_children_num ast_num_of_given_children

#define node_information ast_location

// Eases iterating forward in AST_NODE_LISTs
#define for_each_element(list, iter) \
    iter = (list); while (ASTSon0(iter) != NULL) iter = ASTSon0(iter); \
    for(; iter != NULL; iter = (iter != (list)) ? ASTParent(iter) : NULL)

// Name of operators
#define STR_OPERATOR_ADD "operator +"
#define STR_OPERATOR_MULT "operator *"
#define STR_OPERATOR_DIV "operator *"
#define STR_OPERATOR_MOD "operator %"
#define STR_OPERATOR_MINUS "operator -"
#define STR_OPERATOR_SHIFT_LEFT "operator <<"
#define STR_OPERATOR_SHIFT_RIGHT "operator >>"
#define STR_OPERATOR_LOWER_THAN "operator <"
#define STR_OPERATOR_GREATER_THAN "operator >"
#define STR_OPERATOR_LOWER_EQUAL "operator <="
#define STR_OPERATOR_GREATER_EQUAL "operator >="
#define STR_OPERATOR_EQUAL "operator =="
#define STR_OPERATOR_DIFFERENT "operator !="
#define STR_OPERATOR_BIT_AND "operator &"
#define STR_OPERATOR_BIT_OR "operator |"
#define STR_OPERATOR_BIT_XOR "operator ^"
#define STR_OPERATOR_LOGIC_AND "operator &&"
#define STR_OPERATOR_LOGIC_OR "operator ||"
#define STR_OPERATOR_DERREF STR_OPERATOR_MULT
#define STR_OPERATOR_UNARY_PLUS STR_OPERATOR_ADD
#define STR_OPERATOR_UNARY_NEG STR_OPERATOR_MINUS
#define STR_OPERATOR_LOGIC_NOT "operator !"
#define STR_OPERATOR_BIT_NOT "operator ~"
#define STR_OPERATOR_ASSIGNMENT "operator ="
#define STR_OPERATOR_ADD_ASSIGNMENT "operator +="
#define STR_OPERATOR_MINUS_ASSIGNMENT "operator -="
#define STR_OPERATOR_MUL_ASSIGNMENT "operator *="
#define STR_OPERATOR_DIV_ASSIGNMENT "operator /="
#define STR_OPERATOR_SHL_ASSIGNMENT "operator <<="
#define STR_OPERATOR_SHR_ASSIGNMENT "operator >>="
#define STR_OPERATOR_AND_ASSIGNMENT "operator &="
#define STR_OPERATOR_OR_ASSIGNMENT "operator |="
#define STR_OPERATOR_XOR_ASSIGNMENT "operator ^="
#define STR_OPERATOR_MOD_ASSIGNMENT "operator %="
#define STR_OPERATOR_PREINCREMENT "operator ++"
#define STR_OPERATOR_POSTINCREMENT STR_OPERATOR_PREINCREMENT
#define STR_OPERATOR_PREDECREMENT "operator --"
#define STR_OPERATOR_POSTDECREMENT STR_OPERATOR_PREDECREMENT
#define STR_OPERATOR_COMMA "operator ,"
#define STR_OPERATOR_NEW "operator new"
#define STR_OPERATOR_NEW_ARRAY "operator new[]"
#define STR_OPERATOR_DELETE "operator delete"
#define STR_OPERATOR_DELETE_ARRAY "operator delete[]"
#define STR_OPERATOR_ARROW "operator ->"
#define STR_OPERATOR_ARROW_POINTER "operator ->*"
#define STR_OPERATOR_CALL "operator ()"
#define STR_OPERATOR_SUBSCRIPT "operator []"

MCXX_END_DECLS

#endif // CXX_AST_H
