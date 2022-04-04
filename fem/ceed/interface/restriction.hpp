// Copyright (c) 2010-2022, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#ifndef MFEM_LIBCEED_RESTR
#define MFEM_LIBCEED_RESTR

#include "ceed.hpp"

namespace mfem
{

namespace ceed
{

#ifdef MFEM_USE_CEED
/// @brief Initialize a strided CeedElemRestriction
/** @a nelem is the number of elements,
    @a nqpts is the total number of quadrature points
    @a qdatasize is the number of data per quadrature point
    @a strides Array for strides between [nodes, components, elements].
    Data for node i, component j, element k can be found in the L-vector at
    index i*strides[0] + j*strides[1] + k*strides[2]. CEED_STRIDES_BACKEND may
    be used with vectors created by a Ceed backend. */
void InitStridedRestriction(const mfem::FiniteElementSpace &fes,
                            CeedInt nelem, CeedInt nqpts, CeedInt qdatasize,
                            const CeedInt *strides,
                            CeedElemRestriction *restr);

void InitRestriction(const FiniteElementSpace &fes,
                     const IntegrationRule &irm,
                     Ceed ceed,
                     CeedElemRestriction *restr);

void InitTensorRestriction(const FiniteElementSpace &fes,
                           Ceed ceed, CeedElemRestriction *restr);

void InitRestrictionWithIndices(const FiniteElementSpace &fes,
                                int nelem,
                                const int* indices,
                                Ceed ceed,
                                CeedElemRestriction *restr);


#endif

} // namespace ceed

} // namespace mfem

#endif // MFEM_LIBCEED_RESTR