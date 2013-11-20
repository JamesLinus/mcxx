/*--------------------------------------------------------------------
  (C) Copyright 2006-2012 Barcelona Supercomputing Center
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

#ifndef KNC_VECTOR_LEGALIZATION_HPP
#define KNC_VECTOR_LEGALIZATION_HPP

#include "tl-nodecl-base.hpp"
#include "tl-nodecl-visitor.hpp"
#include <list>

#define MASK_BIT_SIZE 16
#define KNC_VECTOR_LENGTH 64

namespace TL
{
    namespace Vectorization
    {
        class KNCVectorLegalization : public Nodecl::ExhaustiveVisitor<void>
        {
            private:
                const unsigned int _vector_length;
                std::list<Nodecl::NodeclBase> _old_m512;

            public:

                KNCVectorLegalization();

                virtual void visit(const Nodecl::ObjectInit& node);
 
                virtual void visit(const Nodecl::VectorConversion& node);

                virtual void visit(const Nodecl::VectorGather& node);
                virtual void visit(const Nodecl::VectorScatter& node);

                virtual Nodecl::ExhaustiveVisitor<void>::Ret unhandled_node(const Nodecl::NodeclBase& n);
        };

        class KNCStrideVisitorConv : public Nodecl::NodeclVisitor<void>
        {
            private:
                unsigned int _vector_num_elements;

            public:
                KNCStrideVisitorConv(unsigned int vector_num_elements);
        //        virtual void visit(const Nodecl::VectorConversion& node);

                Nodecl::NodeclVisitor<void>::Ret unhandled_node(const Nodecl::NodeclBase& n);

        };
    }
}

#endif // KNC_VECTOR_LEGALIZATION_HPP
