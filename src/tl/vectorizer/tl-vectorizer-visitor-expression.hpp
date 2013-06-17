/*--------------------------------------------------------------------
  (C) Copyright 2006-2013 Barcelona Supercomputing Center
                          Centro Nacional de Supercomputacion
  
  This file is part of Mercurium C/C++ source-to-source compiler.
  
  See AUTHORS file in the top level directory for information
  regarding developers and contributors.
  
  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 3 of the License, or (at your option) any later version.
  
  Mercurium C/C++ source-to-source compiler is distributed in the hope
  that it will be useful, but WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the GNU Lesser General Public License for more
  details.
  
  You should have received a copy of the GNU Lesser General Public
  License along with Mercurium C/C++ source-to-source compiler; if
  not, write to the Free Software Foundation, Inc., 675 Mass Ave,
  Cambridge, MA 02139, USA.
--------------------------------------------------------------------*/

#ifndef TL_VECTORIZER_VISITOR_EXPRESSION_HPP
#define TL_VECTORIZER_VISITOR_EXPRESSION_HPP

#include "tl-nodecl-visitor.hpp"
#include "tl-analysis-static-info.hpp"
#include "tl-vectorizer.hpp"

namespace TL 
{ 
    namespace Vectorization 
    {
        class VectorizerVisitorExpression : public Nodecl::NodeclVisitor<void>
        {
            private:
                VectorizerEnvironment& _environment;

                bool is_declared_in_scope(const scope_t *const target_scope , 
                        const scope_t *const symbol_scope) const;

            public:
                VectorizerVisitorExpression(VectorizerEnvironment& environment);

                virtual void visit(const Nodecl::Add& n);
                virtual void visit(const Nodecl::Minus& n);
                virtual void visit(const Nodecl::Mul& n);
                virtual void visit(const Nodecl::Div& n);
                virtual void visit(const Nodecl::Neg& n);

                virtual void visit(const Nodecl::LowerThan& n);
                virtual void visit(const Nodecl::GreaterThan& n);
                virtual void visit(const Nodecl::Equal& n);
                virtual void visit(const Nodecl::BitwiseAnd& n);
                virtual void visit(const Nodecl::BitwiseOr& n);
                virtual void visit(const Nodecl::LogicalAnd& n);
                virtual void visit(const Nodecl::LogicalOr& n);
                virtual void visit(const Nodecl::ConditionalExpression& n);

                virtual void visit(const Nodecl::Assignment& n);
                virtual void visit(const Nodecl::AddAssignment& n);
                virtual void visit(const Nodecl::MinusAssignment& n);
                virtual void visit(const Nodecl::MulAssignment& n);
                virtual void visit(const Nodecl::DivAssignment& n);

                virtual void visit(const Nodecl::Conversion& n);
                virtual void visit(const Nodecl::Cast& n);
                virtual void visit(const Nodecl::ArraySubscript& n);
                virtual void visit(const Nodecl::FunctionCall& n);
                virtual void visit(const Nodecl::Symbol& n);
                virtual void visit(const Nodecl::IntegerLiteral& n);
                virtual void visit(const Nodecl::FloatingLiteral& n);
                
                virtual void visit(const Nodecl::Reference& n);

                Nodecl::NodeclVisitor<void>::Ret unhandled_node(const Nodecl::NodeclBase& n);
        };
    }
}

#endif //TL_VECTORIZER_VISITOR_EXPRESSION_HPP
