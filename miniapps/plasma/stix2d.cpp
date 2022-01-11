// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.
//
//   -----------------------------------------------------------------------
//       Stix2D Miniapp: Cold Plasma Electromagnetic Simulation Code
//   -----------------------------------------------------------------------
//
//   Assumes that all sources and boundary conditions oscillate with the same
//   frequency although not necessarily in phase with one another.  This
//   assumption implies that we can factor out the time dependence which we
//   take to be of the form exp(i omega t).  With these assumptions we can
//   write the Maxwell equations in the form:
//
//   -i omega epsilon E = Curl mu^{-1} B - J
//    i omega B         = Curl E
//
//   Which combine to yield:
//
//   Curl mu^{-1} Curl E - omega^2 epsilon E = i omega J
//
//   In a cold plasma the dielectric tensor, epsilon, is complex-valued and
//   anisotropic.  The anisotropy aligns with the external magnetic field and
//   the values depend on the properties of the plasma including the masses and
//   charges of its constituent ion species.
//
//   For a magnetic field aligned with the z-axis the dielectric tensor has
//   the form:
//              | S  -iD 0 |
//    epsilon = |iD   S  0 |
//              | 0   0  P |
//
//   Where:
//      S = 1 - Sum_species omega_p^2 / (omega^2 - omega_c^2)
//      D = Sum_species omega_p^2 omega_c / (omega^2 - omega_c^2)
//      P = 1 - Sum_species omega_p^2 / omega^2
//
//   and:
//      omega_p is the plasma frequency
//      omega_c is the cyclotron frequency
//      omega   is the driving frequency
//
//   The plasma and cyclotron frequencies depend primarily on the properties
//   of the ion species.  We also include a complex-valued mass correction
//   which depends on the plasma temperature.
//
//   We discretize this equation with H(Curl) a.k.a Nedelec basis
//   functions.  The curl curl operator must be handled with
//   integration by parts which yields a surface integral:
//
//   (W, Curl mu^{-1} Curl E) = (Curl W, mu^{-1} Curl E)
//               + (W, n x (mu^{-1} Curl E))_{\Gamma}
//
//   or
//
//   (W, Curl mu^{-1} Curl E) = (Curl W, mu^{-1} Curl E)
//               - i omega (W, n x H)_{\Gamma}
//
//   For plane waves
//     omega B = - k x E
//     omega D = k x H, assuming n x k = 0 => n x H = omega epsilon E / |k|
//
//   c = omega/|k|
//
//   (W, Curl mu^{-1} Curl E) = (Curl W, mu^{-1} Curl E)
//               - i omega sqrt{epsilon/mu} (W, E)_{\Gamma}
//
// (By default the sources and fields are all zero)
//
// Compile with: make stix2d
//
// Sample runs:
//   ./stix2d -rod '0 0 1 0 0 0.1' -o 3 -s 1 -rs 0 -maxit 1 -f 1e6
//
// Sample runs with partial assembly:
//   ./stix2d -rod '0 0 1 0 0 0.1' -o 3 -s 1 -rs 0 -maxit 1 -f 1e6 -pa
//
// Device sample runs:
//   ./stix2d -rod '0 0 1 0 0 0.1' -o 3 -s 1 -rs 0 -maxit 1 -f 1e6 -pa -d cuda
//
// Parallel sample runs:
//   mpirun -np 4 ./stix2d -rod '0 0 1 0 0 0.1' -dbcs '1' -w Z -o 3 -s 1 -rs 0 -maxit 1 -f 1e6
//

#include "cold_plasma_dielectric_coefs.hpp"
#include "cold_plasma_dielectric_solver.hpp"
#include "../common/mesh_extras.hpp"
#include <fstream>
#include <iostream>
#include <cmath>
#include <complex>

using namespace std;
using namespace mfem;
using namespace mfem::common;
using namespace mfem::plasma;

// Admittance for Absorbing Boundary Condition
Coefficient * SetupAdmittanceCoefficient(const Mesh & mesh,
                                        const Array<int> & abcs);

// Storage for user-supplied, real-valued impedance
static Vector pw_eta_(0);      // Piecewise impedance values
static Vector pw_eta_inv_(0);  // Piecewise inverse impedance values

// Storage for user-supplied, complex-valued impedance
//static Vector pw_eta_re_(0);      // Piecewise real impedance
//static Vector pw_eta_inv_re_(0);  // Piecewise inverse real impedance
//static Vector pw_eta_im_(0);      // Piecewise imaginary impedance
//static Vector pw_eta_inv_im_(0);  // Piecewise inverse imaginary impedance

// Current Density Function
static Vector rod_params_
(0); // Amplitude of x, y, z current source, position in 2D, and radius
static Vector slab_params_
(0); // Amplitude of x, y, z current source, position in 2D, and size in 2D
static int slab_profile_;

void rod_current_source_r(const Vector &x, Vector &j);
void rod_current_source_i(const Vector &x, Vector &j);
void slab_current_source_r(const Vector &x, Vector &j);
void slab_current_source_i(const Vector &x, Vector &j);
void j_src_r(const Vector &x, Vector &j)
{
   if (rod_params_.Size() > 0)
   {
      rod_current_source_r(x, j);
   }
   else if (slab_params_.Size() > 0)
   {
      slab_current_source_r(x, j);
   }
}
void j_src_i(const Vector &x, Vector &j)
{
   if (rod_params_.Size() > 0)
   {
      rod_current_source_i(x, j);
   }
   else if (slab_params_.Size() > 0)
   {
      slab_current_source_i(x, j);
   }
}

// Electric Field Boundary Condition: The following function returns zero but
// any function could be used.
void e_bc_r(const Vector &x, Vector &E);
void e_bc_i(const Vector &x, Vector &E);

/*
class ColdPlasmaPlaneWave: public VectorCoefficient
{
public:
   ColdPlasmaPlaneWave(char type,
                       double omega,
                       const Vector & B,
                       const Vector & number,
                       const Vector & charge,
                       const Vector & mass,
                       bool realPart = true);

   void SetCurrentSlab(double Jy, double xJ, double delta, double Lx)
   { Jy_ = Jy; xJ_ = xJ; dx_ = delta, Lx_ = Lx; }

   void SetPhaseShift(const Vector &k) { k_ = k; }

   void Eval(Vector &V, ElementTransformation &T,
             const IntegrationPoint &ip);

private:
   char type_;
   bool realPart_;
   double omega_;
   double Bmag_;
   double Jy_;
   double xJ_;
   double dx_;
   double Lx_;
   Vector k_;

   const Vector & B_;
   const Vector & numbers_;
   const Vector & charges_;
   const Vector & masses_;

   double S_;
   double D_;
   double P_;
};
*/
class MultiStrapAntennaH : public VectorCoefficient
{
private:
   bool real_part_;
   int num_straps_;
   double tol_;
   Vector params_;
   Vector x_;

public:
   MultiStrapAntennaH(int n, const Vector &params,
                      bool real_part, double tol = 1e-6)
      : VectorCoefficient(3), real_part_(real_part), num_straps_(n),
        tol_(tol), params_(params), x_(2)
   {
      MFEM_ASSERT(params.Size() == 10 * n,
                  "Incorrect number of parameters provided to "
                  "MultiStrapAntennaH");
   }

   void Eval(Vector &V, ElementTransformation &T,
             const IntegrationPoint &ip)
   {
      V.SetSize(3); V = 0.0;
      T.Transform(ip, x_);
      for (int i=0; i<num_straps_; i++)
      {
          double x0  = params_[10 * i + 0];
          double y0  = params_[10 * i + 1];
          double x1  = params_[10 * i + 2];
          double y1  = params_[10 * i + 3];
          double x2  = params_[10 * i + 4];
          double y2  = params_[10 * i + 5];
          double x3  = params_[10 * i + 6];
          double y3  = params_[10 * i + 7];

          double ReI = params_[10 * i + 8];
          double ImI = params_[10 * i + 9];

          double d01 = sqrt(pow(x1 - x0, 2) + pow(y1 - y0, 2));
          double d12 = sqrt(pow(x2 - x1, 2) + pow(y2 - y1, 2));
          double d23 = sqrt(pow(x3 - x2, 2) + pow(y3 - y2, 2));
          double d30 = sqrt(pow(x0 - x3, 2) + pow(y0 - y3, 2));

          double   H = (real_part_ ? ReI : ImI) / (d01 + d12 + d23 + d30);
          
          // *** The following will break on any vertical sides ***
           // Bottom of Antenna Strap:
           double s1 = (y1-y0)/(x1-x0);
           double b1 = y1 - s1*x1;
           // Right of Antenna Strap:
           double s2 = (y2-y1)/(x2-x1);
           double b2 = y2 - s2*x2;
           // Top of Antenna Strap:
           double s3 = (y3-y2)/(x3-x2);
           double b3 = y3 - s3*x3;
           // Left of Antenna Strap:
           double s4 = (y3-y0)/(x3-x0);
           double b4 = y3 - s4*x3;

           if (fabs(x_[1] - (s1*x_[0]+b1)) <= tol_
               && x_[0] >= x0 && x_[0] <= x1)
           {
              V[0] = (x1 - x0) * H / d01;
              V[1] = (y1 - y0) * H / d01;
              break;
           }
           else if (fabs(x_[1] - (s2*x_[0]+b2)) <= tol_
                    && x_[1] >= y1 && x_[1] <= y2)
           {
              V[0] = (x2 - x1) * H / d12;
              V[1] = (y2 - y1) * H / d12;
              break;
           }
           else if (fabs(x_[1] - (s3*x_[0]+b3)) <= tol_
                    && x_[0] >= x3 && x_[0] <= x2)
           {
              V[0] = (x3 - x2) * H / d23;
              V[1] = (y3 - y2) * H / d23;
              break;
           }
           else if (fabs(x_[1] - (s4*x_[0]+b4)) <= tol_
                    && x_[1] >= y0 && x_[1] <= y3)
           {
              V[0] = (x0 - x3) * H / d30;
              V[1] = (y0 - y3) * H / d30;
              break;
           }
      }
   }
};
void Update(ParFiniteElementSpace & H1FESpace,
            ParFiniteElementSpace & HCurlFESpace,
            ParFiniteElementSpace & HDivFESpace,
            ParFiniteElementSpace & L2FESpace,
            ParGridFunction & BField,
            VectorCoefficient & BCoef,
            Coefficient & rhoCoef,
            Coefficient & TCoef,
            Coefficient & xposCoef,
            int & size_h1,
            int & size_l2,
            Array<int> & density_offsets,
            Array<int> & temperature_offsets,
            BlockVector & density,
            BlockVector & temperature,
            ParGridFunction & density_gf,
            ParGridFunction & temperature_gf,
            ParGridFunction & xposition_gf);

//static double freq_ = 1.0e9;

// Mesh Size
//static Vector mesh_dim_(0); // x, y, z dimensions of mesh

// Prints the program's logo to the given output stream
void display_banner(ostream & os);

int main(int argc, char *argv[])
{
   MPI_Session mpi(argc, argv);
   if (!mpi.Root()) { mfem::out.Disable(); mfem::err.Disable(); }

   display_banner(mfem::out);


   int logging = 1;

   // Parse command-line options.
   const char *mesh_file = "ellipse_origin_h0pt0625_o3.mesh";
   int ser_ref_levels = 0;
   int order = 1;
   int maxit = 100;
   int sol = 2;
   int prec = 1;
   // int nspecies = 2;
   bool herm_conv = false;
   bool vis_u = false;
   bool visualization = true;
   bool visit = true;

   double freq = 1.0e6;
   const char * wave_type = "R";

   Vector BVec(3);
   BVec = 0.0; BVec(0) = 0.1;

   bool phase_shift = false;
   Vector kVec(3);
   kVec = 0.0;
   //Vector kReVec;
   //Vector kImVec;

   double hz = -1.0;

   Vector numbers;
   Vector charges;
   Vector masses;
   Vector temps;
   Vector minority;
   Vector temp_charges;
   double xpositions = 0;

   PlasmaProfile::Type dpt = PlasmaProfile::CONSTANT;
   PlasmaProfile::Type tpt = PlasmaProfile::CONSTANT;
   PlasmaProfile::Type xpt = PlasmaProfile::GRADIENT;
   BFieldProfile::Type bpt = BFieldProfile::CONSTANT;
   Vector dpp;
   Vector tpp;
   Vector bpp;
   Vector xpp(7);
   xpp = 0.0; xpp(4) = 1.0;
   int nuprof = 0;

   Array<int> abcs; // Absorbing BC attributes
   Array<int> sbca; // Sheath BC attributes
   Array<int> peca; // Perfect Electric Conductor BC attributes
   Array<int> dbca1; // Dirichlet BC attributes
   Array<int> dbca2; // Dirichlet BC attributes
   Array<int> nbcas; // Neumann BC attributes for multi-strap antenna source
   Array<int> nbca1; // Neumann BC attributes
   Array<int> nbca2; // Neumann BC attributes
   Vector dbcv1; // Dirichlet BC values
   Vector dbcv2; // Dirichlet BC values
   Vector nbcv1; // Neumann BC values
   Vector nbcv2; // Neumann BC values
    
   int msa_n = 0;
   Vector msa_p(0);

   int num_elements = 10;

   SolverOptions solOpts;
   solOpts.maxIter = 1000;
   solOpts.kDim = 50;
   solOpts.printLvl = 1;
   solOpts.relTol = 1e-4;
   solOpts.euLvl = 1;
    
   bool logo = false;
   bool per_y = false;
   bool check_eps_inv = false;
   bool pa = false;
   const char *device_config = "cpu";

   OptionsParser args(argc, argv);
   args.AddOption(&logo, "-logo", "--print-logo", "-no-logo",
                   "--no-print-logo", "Print logo and exit.");
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&per_y, "-per-y", "--periodic-in-y", "-no-per-y",
                   "--not-periodic-in-y",
                   "The input mesh is periodic in the y-direction.");
   args.AddOption(&ser_ref_levels, "-rs", "--refine-serial",
                  "Number of times to refine the mesh uniformly in serial.");
   args.AddOption(&order, "-o", "--order",
                  "Finite element order (polynomial degree).");
   // args.AddOption(&nspecies, "-ns", "--num-species",
   //               "Number of ion species.");
   args.AddOption(&freq, "-f", "--frequency",
                  "Frequency in Hertz (of course...)");
   args.AddOption(&hz, "-mh", "--mesh-height",
                  "Thickness of extruded mesh in meters.");
   args.AddOption((int*)&dpt, "-dp", "--density-profile",
                  "Density Profile Type (for ions): \n"
                  "0 - Constant, 1 - Constant Gradient, "
                  "2 - Hyprebolic Tangent, 3 - Elliptic Cosine.");
   args.AddOption(&dpp, "-dpp", "--density-profile-params",
                  "Density Profile Parameters:\n"
                  "   CONSTANT: density value\n"
                  "   GRADIENT: value, location, gradient (7 params)\n"
                  "   TANH:     value at 0, value at 1, skin depth, "
                  "location of 0 point, unit vector along gradient, "
                  "   ELLIPTIC_COS: value at -1, value at 1, "
                  "radius in x, radius in y, location of center.");
   args.AddOption((int*)&bpt, "-bp", "--Bfield-profile",
                   "BField Profile Type: \n"
                   "0 - Constant, 1 - Constant Gradient, "
                   "2 - Hyprebolic Tangent, 3 - Elliptic Cosine.");
   args.AddOption(&bpp, "-bpp", "--Bfield-profile-params",
                   "BField Profile Parameters:\n"
                   "  B_P: value at -1, value at 1, "
                   "radius in x, radius in y, location of center, Bz, placeholder.");
   args.AddOption((int*)&tpt, "-tp", "--temperature-profile",
                  "Temperature Profile Type: \n"
                  "0 - Constant, 1 - Constant Gradient, "
                  "2 - Hyperbolic Tangent, 3 - Elliptic Cosine.");
   args.AddOption(&tpp, "-tpp", "--temperature-profile-params",
                  "Temperature Profile Parameters: \n"
                  "   CONSTANT: temperature value \n"
                  "   GRADIENT: value, location, gradient (7 params)\n"
                  "   TANH:     value at 0, value at 1, skin depth, "
                  "location of 0 point, unit vector along gradient, "
                  "   ELLIPTIC_COS: value at -1, value at 1, "
                  "radius in x, radius in y, location of center.");
   args.AddOption(&nuprof, "-nuprof", "--collisional-profile",
                   "Temperature Profile Type: \n"
                   "0 - Standard e-i Collision Freq, 1 - Custom Freq.");
   args.AddOption(&wave_type, "-w", "--wave-type",
                  "Wave type: 'R' - Right Circularly Polarized, "
                  "'L' - Left Circularly Polarized, "
                  "'O' - Ordinary, 'X' - Extraordinary, "
                  "'J' - Current Slab (in conjunction with -slab), "
                  "'Z' - Zero");
   args.AddOption(&BVec, "-B", "--magnetic-flux",
                  "Background magnetic flux vector");
   args.AddOption(&kVec, "-k-vec", "--phase-vector",
                   "Phase shift vector across periodic directions."
                   " For complex phase shifts input 3 real phase shifts "
                   "followed by 3 imaginary phase shifts");
   args.AddOption(&msa_n, "-ns", "--num-straps","");
   args.AddOption(&msa_p, "-sp", "--strap-params","");
   // args.AddOption(&numbers, "-num", "--number-densites",
   //               "Number densities of the various species");
   args.AddOption(&charges, "-q", "--charges",
                  "Charges of the various species "
                  "(in units of electron charge)");
   //args.AddOption(&masses, "-mass", "--masses",
   //"Masses of the various species (in amu)");
   args.AddOption(&minority, "-min", "--minority",
                   "Minority Ion Species: charge, mass (amu), concentration."
                   " Concentration being: n_min/n_e");
   args.AddOption(&prec, "-pc", "--precond",
                   "Preconditioner: 1 - Diagonal Scaling, 2 - ParaSails, "
                   "3 - Euclid, 4 - AMS");
   args.AddOption(&sol, "-s", "--solver",
                   "Solver: 1 - GMRES, 2 - FGMRES, 3 - MINRES"
#ifdef MFEM_USE_SUPERLU
                  ", 4 - SuperLU"
#endif
#ifdef MFEM_USE_STRUMPACK
                  ", 5 - STRUMPACK"
#endif
                 );
   args.AddOption(&solOpts.maxIter, "-sol-it", "--solver-iterations",
                  "Maximum number of solver iterations.");
   args.AddOption(&solOpts.kDim, "-sol-k-dim", "--solver-krylov-dimension",
                  "Krylov space dimension for GMRES and FGMRES.");
   args.AddOption(&solOpts.relTol, "-sol-tol", "--solver-tolerance",
                  "Relative tolerance for GMRES or FGMRES.");
   args.AddOption(&solOpts.printLvl, "-sol-prnt-lvl", "--solver-print-level",
                  "Logging level for solvers.");
   args.AddOption(&solOpts.euLvl, "-eu-lvl", "--euclid-level",
                  "Euclid factorization level for ILU(k).");
   args.AddOption(&pw_eta_, "-pwz", "--piecewise-eta",
                  "Piecewise values of Impedance (one value per abc surface)");
    /*
    args.AddOption(&pw_eta_re_, "-pwz-r", "--piecewise-eta-r",
                   "Piecewise values of Real part of Complex Impedance "
                   "(one value per abc surface)");
    args.AddOption(&pw_eta_im_, "-pwz-i", "--piecewise-eta-i",
                   "Piecewise values of Imaginary part of Complex Impedance "
                   "(one value per abc surface)");
    */
   args.AddOption(&rod_params_, "-rod", "--rod_params",
                  "3D Vector Amplitude, 2D Position, Radius");
   args.AddOption(&slab_params_, "-slab", "--slab_params",
                  "3D Vector Amplitude, 2D Position, 2D Size");
   args.AddOption(&slab_profile_, "-slab-prof", "--slab_profile",
                   "0 (Constant) or 1 (Sin Function)");
   args.AddOption(&abcs, "-abcs", "--absorbing-bc-surf",
                  "Absorbing Boundary Condition Surfaces");
   args.AddOption(&sbca, "-sbcs", "--sheath-bc-surf",
                  "Sheath Boundary Condition Surfaces");
   args.AddOption(&peca, "-pecs", "--pec-bc-surf",
                  "Perfect Electrical Conductor Boundary Condition Surfaces");
   args.AddOption(&dbca1, "-dbcs1", "--dirichlet-bc-1-surf",
                   "Dirichlet Boundary Condition Surfaces Using Value 1");
   args.AddOption(&dbca2, "-dbcs2", "--dirichlet-bc-2-surf",
                   "Dirichlet Boundary Condition Surfaces Using Value 2");
   args.AddOption(&dbcv1, "-dbcv1", "--dirichlet-bc-1-vals",
                   "Dirichlet Boundary Condition Value 1 (v_x v_y v_z)"
                   " or (Re(v_x) Re(v_y) Re(v_z) Im(v_x) Im(v_y) Im(v_z))");
   args.AddOption(&dbcv2, "-dbcv2", "--dirichlet-bc-2-vals",
                   "Dirichlet Boundary Condition Value 2 (v_x v_y v_z)"
                   " or (Re(v_x) Re(v_y) Re(v_z) Im(v_x) Im(v_y) Im(v_z))");
   args.AddOption(&nbcas, "-nbcs-msa", "--neumann-bc-straps",
                    "Neumann Boundary Condition Surfaces Using "
                    "Multi-Strap Antenna");
   args.AddOption(&nbca1, "-nbcs1", "--neumann-bc-1-surf",
                   "Neumann Boundary Condition Surfaces Using Value 1");
   args.AddOption(&nbca2, "-nbcs2", "--neumann-bc-2-surf",
                   "Neumann Boundary Condition Surfaces Using Value 2");
   args.AddOption(&nbcv1, "-nbcv1", "--neumann-bc-1-vals",
                   "Neuamnn Boundary Condition (surface current) "
                   "Value 1 (v_x v_y v_z) or "
                   "(Re(v_x) Re(v_y) Re(v_z) Im(v_x) Im(v_y) Im(v_z))");
   args.AddOption(&nbcv2, "-nbcv2", "--neumann-bc-2-vals",
                   "Neumann Boundary Condition (surface current) "
                   "Value 2 (v_x v_y v_z) or "
                   "(Re(v_x) Re(v_y) Re(v_z) Im(v_x) Im(v_y) Im(v_z))");
   // args.AddOption(&num_elements, "-ne", "--num-elements",
   //             "The number of mesh elements in x");
   args.AddOption(&maxit, "-maxit", "--max-amr-iterations",
                  "Max number of iterations in the main AMR loop.");
   args.AddOption(&herm_conv, "-herm", "--hermitian", "-no-herm",
                  "--no-hermitian", "Use convention for Hermitian operators.");
   args.AddOption(&vis_u, "-vis-u", "--visualize-energy", "-no-vis-u",
                  "--no-visualize-energy",
                  "Enable or disable visualization of energy density.");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&visit, "-visit", "--visit", "-no-visit", "--no-visit",
                  "Enable or disable VisIt visualization.");
   args.AddOption(&pa, "-pa", "--partial-assembly", "-no-pa",
                  "--no-partial-assembly", "Enable Partial Assembly.");
   args.AddOption(&device_config, "-d", "--device",
                  "Device configuration string, see Device::Configure().");
   args.Parse();
   if (!args.Good())
   {
      if (mpi.Root())
      {
         args.PrintUsage(cout);
      }
      return 1;
   }
   Device device(device_config);
   if (mpi.Root())
   {
      device.Print();
   }
    if (dpp.Size() == 0)
    {
       dpp.SetSize(1);
       dpp[0] = 1.0e19;
    }

    if (charges.Size() == 0)
    {
       if (minority.Size() == 0)
       {
          charges.SetSize(2);
          charges[0] = -1.0;
          charges[1] =  1.0;

          masses.SetSize(2);
          masses[0] = me_u_;
          masses[1] = 2.01410178;
       }
       else
       {
          charges.SetSize(3);
          charges[0] = -1.0;
          charges[1] =  1.0;
          charges[2] = minority[0];

          masses.SetSize(3);
          masses[0] = me_u_;
          masses[1] = 2.01410178;
          masses[2] = minority[1];
       }
    }
    if (minority.Size() == 0)
    {
       if (charges.Size() == 2)
       {
          numbers.SetSize(2);
          masses.SetSize(2);
          masses[0] = me_u_;
          masses[1] = 2.01410178;
          switch (dpt)
          {
             case PlasmaProfile::CONSTANT:
                numbers[0] = dpp[0];
                numbers[1] = dpp[0];
                break;
             case PlasmaProfile::GRADIENT:
                numbers[0] = dpp[0];
                numbers[1] = dpp[0];
                break;
             case PlasmaProfile::TANH:
                numbers[0] = dpp[1];
                numbers[1] = dpp[1];
                break;
             case PlasmaProfile::ELLIPTIC_COS:
                numbers[0] = dpp[1];
                numbers[1] = dpp[1];
                break;
             case PlasmaProfile::PARABOLIC:
                numbers[0] = dpp[1];
                numbers[1] = dpp[1];
                break;
             default:
                numbers[0] = 1.0e19;
                numbers[1] = 1.0e19;
                break;
          }
       }
       else
       {
          numbers.SetSize(3);
          masses.SetSize(3);
          masses[0] = me_u_;
          masses[1] = 2.01410178;
          masses[2] = 3.01604928;
          switch (dpt)
          {
             case PlasmaProfile::CONSTANT:
                numbers[0] = dpp[0];
                numbers[1] = 0.5*dpp[0];
                numbers[2] = 0.5*dpp[0];
                break;
             case PlasmaProfile::GRADIENT:
                numbers[0] = dpp[0];
                numbers[1] = 0.5*dpp[0];
                numbers[2] = 0.5*dpp[0];
                break;
             case PlasmaProfile::TANH:
                numbers[0] = dpp[1];
                numbers[1] = 0.5*dpp[1];
                numbers[2] = 0.5*dpp[1];
                break;
             case PlasmaProfile::ELLIPTIC_COS:
                numbers[0] = dpp[1];
                numbers[1] = 0.5*dpp[1];
                numbers[2] = 0.5*dpp[1];
                break;
             case PlasmaProfile::PARABOLIC:
                numbers[0] = dpp[1];
                numbers[1] = 0.5*dpp[1];
                numbers[2] = 0.5*dpp[1];
                break;
             default:
                numbers[0] = 1.0e19;
                numbers[1] = 0.5*1.0e19;
                numbers[2] = 0.5*1.0e19;
                break;
          }
       }
    }
    if (minority.Size() > 0)
    {
       if (charges.Size() == 2)
       {
          temp_charges.SetSize(3);
          temp_charges[0] = charges[0];
          temp_charges[1] = charges[1];
          temp_charges[2] = minority[0];
          charges.SetSize(3);
          charges = temp_charges;

          numbers.SetSize(3);
          masses.SetSize(3);
          masses[0] = me_u_;
          masses[1] = 2.01410178;
          masses[2] = minority[1];
          switch (dpt)
          {
             case PlasmaProfile::CONSTANT:
                numbers[0] = dpp[0];
                numbers[1] = (1.0-minority[2]*minority[0])*dpp[0];
                numbers[2] = minority[2]*dpp[0];
                break;
             case PlasmaProfile::GRADIENT:
                numbers[0] = dpp[0];
                numbers[1] = (1.0-minority[2]*minority[0])*dpp[0];
                numbers[2] = minority[2]*dpp[0];
                break;
             case PlasmaProfile::TANH:
                numbers[0] = dpp[1];
                numbers[1] = (1.0-minority[2]*minority[0])*dpp[1];
                numbers[2] = minority[2]*dpp[1];
                break;
             case PlasmaProfile::ELLIPTIC_COS:
                numbers[0] = dpp[1];
                numbers[1] = (1.0-minority[2]*minority[0])*dpp[1];
                numbers[2] = minority[2]*dpp[1];
                break;
             case PlasmaProfile::PARABOLIC:
                numbers[0] = dpp[1];
                numbers[1] = (1.0-minority[2]*minority[0])*dpp[1];
                numbers[2] = minority[2]*dpp[1];
                break;
             default:
                numbers[0] = 1.0e19;
                numbers[1] = (1.0-minority[2]*minority[0])*1.0e19;
                numbers[2] = minority[2]*1.0e19;
                break;
          }
       }
       else
       {
          temp_charges.SetSize(4);
          temp_charges[0] = charges[0];
          temp_charges[1] = charges[1];
          temp_charges[2] = charges[2];
          temp_charges[3] = minority[0];
          charges.SetSize(4);
          charges = temp_charges;

          numbers.SetSize(4);
          masses.SetSize(4);
          masses[0] = me_u_;
          masses[1] = 2.01410178;
          masses[2] = 3.01604928;
          masses[3] = minority[1];
          switch (dpt)
          {
             case PlasmaProfile::CONSTANT:
                numbers[0] = dpp[0];
                numbers[1] = 0.5*(1.0-minority[2]*minority[0])*dpp[0];
                numbers[2] = 0.5*(1.0-minority[2]*minority[0])*dpp[0];
                numbers[3] = minority[2]*dpp[0];
                break;
             case PlasmaProfile::GRADIENT:
                numbers[0] = dpp[0];
                numbers[1] = 0.5*(1.0-minority[2]*minority[0])*dpp[0];
                numbers[2] = 0.5*(1.0-minority[2]*minority[0])*dpp[0];
                numbers[3] = minority[2]*dpp[0];
                break;
             case PlasmaProfile::TANH:
                numbers[0] = dpp[1];
                numbers[1] = 0.5*(1.0-minority[2]*minority[0])*dpp[1];
                numbers[2] = 0.5*(1.0-minority[2]*minority[0])*dpp[1];
                numbers[3] = minority[2]*dpp[1];
                break;
             case PlasmaProfile::ELLIPTIC_COS:
                numbers[0] = dpp[1];
                numbers[1] = 0.5*(1.0-minority[2]*minority[0])*dpp[1];
                numbers[2] = 0.5*(1.0-minority[2]*minority[0])*dpp[1];
                numbers[3] = minority[2]*dpp[1];
                break;
             case PlasmaProfile::PARABOLIC:
                numbers[0] = dpp[1];
                numbers[1] = 0.5*(1.0-minority[2]*minority[0])*dpp[1];
                numbers[2] = 0.5*(1.0-minority[2]*minority[0])*dpp[1];
                numbers[3] = minority[2]*dpp[1];
                break;
             default:
                numbers[0] = 1.0e19;
                numbers[1] = 0.5*(1.0-minority[2]*minority[0])*1.0e19;
                numbers[2] = 0.5*(1.0-minority[2]*minority[0])*1.0e19;
                numbers[3] = minority[2]*1.0e19;
                break;
          }
       }
    }
    if (temps.Size() == 0)
    {
       temps.SetSize(numbers.Size());
       if (tpp.Size() == 0)
       {
          tpp.SetSize(1);
          tpp[0] = 1.0e3;
          for (int i=0; i<numbers.Size(); i++) {temps[i] = tpp[0];}
       }
       else
       {
          switch (tpt)
          {
             case PlasmaProfile::CONSTANT:
                for (int i=0; i<numbers.Size(); i++) {temps[i] = tpp[0];}
                break;
             case PlasmaProfile::GRADIENT:
                for (int i=0; i<numbers.Size(); i++) {temps[i] = tpp[0];}
                break;
             case PlasmaProfile::TANH:
                for (int i=0; i<numbers.Size(); i++) {temps[i] = tpp[1];}
                break;
             case PlasmaProfile::ELLIPTIC_COS:
                for (int i=0; i<numbers.Size(); i++) {temps[i] = tpp[1];}
                break;
             case PlasmaProfile::PARABOLIC:
                for (int i=0; i<numbers.Size(); i++) {temps[i] = tpp[1];}
                break;
             default:
                for (int i=0; i<numbers.Size(); i++) {temps[i] = 1e3;}
                break;
          }
       }
    }
   if (num_elements <= 0)
   {
      num_elements = 10;
   }
   if (hz < 0.0)
   {
      hz = 0.1;
   }
   double omega = 2.0 * M_PI * freq;
    if (kVec.Size() != 0)
    {
       phase_shift = true;
    }

   if (mpi.Root())
   {
      args.PrintOptions(cout);
   }

   ComplexOperator::Convention conv =
      herm_conv ? ComplexOperator::HERMITIAN : ComplexOperator::BLOCK_SYMMETRIC;

   if (mpi.Root())
   {
      double lam0 = c0_ / freq;
      double Bmag = BVec.Norml2();
      std::complex<double> S = S_cold_plasma(omega, Bmag, xpositions, numbers,
                                              charges, masses, temps, nuprof);
      std::complex<double> P = P_cold_plasma(omega, xpositions, numbers,
                                              charges, masses, temps, nuprof);
      std::complex<double> D = D_cold_plasma(omega, Bmag, xpositions, numbers,
                                              charges, masses, temps, nuprof);
      std::complex<double> R = R_cold_plasma(omega, Bmag, xpositions, numbers,
                                              charges, masses, temps, nuprof);
      std::complex<double> L = L_cold_plasma(omega, Bmag, xpositions, numbers,
                                              charges, masses, temps, nuprof);

      cout << "\nConvenient Terms:\n";
      cout << "R = " << R << ",\tL = " << L << endl;
      cout << "S = " << S << ",\tD = " << D << ",\tP = " << P << endl;

      cout << "\nSpecies Properties (number, charge, mass):\n";
      for (int i=0; i<numbers.Size(); i++)
      {
         cout << numbers[i] << '\t' << charges[i] << '\t' << masses[i] << '\n';
      }
      cout << "\nPlasma and Cyclotron Frequencies by Species (GHz):\n";
      for (int i=0; i<numbers.Size(); i++)
      {
         cout << omega_p(numbers[i], charges[i], masses[i]) / (2.0e9 * M_PI)
              << '\t'
              << omega_c(Bmag, charges[i], masses[i]) / (2.0e9 * M_PI) << '\n';
      }

      cout << "\nWavelengths (meters):\n";
      cout << "   Free Space Wavelength: " << lam0 << '\n';
      /*
      if (S < D)
      {
         cout << "   Decaying L mode:       " << lam0 / sqrt(D-S) << '\n';
      }
      else
      {
         cout << "   Oscillating L mode:    " << lam0 / sqrt(S-D) << '\n';
      }
      if (S < - D)
      {
         cout << "   Decaying R mode:       " << lam0 / sqrt(-S-D) << '\n';
      }
      else
      {
         cout << "   Oscillating R mode:    " << lam0 / sqrt(S+D) << '\n';
      }
      if (P < 0)
      {
         cout << "   Decaying O mode:       " << lam0 / sqrt(-P) << '\n';
      }
      else
      {
         cout << "   Oscillating O mode:    " << lam0 / sqrt(P) << '\n';
      }
      if ((S * S - D * D) / S < 0)
      {
         cout << "   Decaying X mode:       " << lam0 * sqrt(-S/(S*S-D*D))
              << '\n';
      }
      else
      {
         cout << "   Oscillating X mode:    " << lam0 * sqrt(S/(S*S-D*D))
              << '\n';
      }
      */
      cout << endl;
   }

   // Read the (serial) mesh from the given mesh file on all processors.  We
   // can handle triangular, quadrilateral, tetrahedral, hexahedral, surface
   // and volume meshes with the same code.
   if ( mpi.Root() && logging > 0 )
   {
      cout << "Building Extruded 2D Mesh ..." << endl;
   }

   tic_toc.Clear();
   tic_toc.Start();

   Mesh * mesh2d = new Mesh(mesh_file, 1, 1);
   for (int lev = 0; lev < ser_ref_levels; lev++)
   {
      mesh2d->UniformRefinement();
   }
   Mesh * mesh = Extrude2D(mesh2d, 3, hz);
   delete mesh2d;
    {
       Array<int> v2v(mesh->GetNV());
       for (int i=0; i<v2v.Size(); i++) { v2v[i] = i; }
       for (int i=0; i<mesh->GetNV() / 4; i++) { v2v[4 * i + 3] = 4 * i; }

       Mesh * per_mesh = MakePeriodicMesh(mesh, v2v);
       delete mesh;
       mesh = per_mesh;
    }
   tic_toc.Stop();

   if (mpi.Root() && logging > 0 )
   {
      cout << " done in " << tic_toc.RealTime() << " seconds." << endl;
   }

   // Ensure that quad and hex meshes are treated as non-conforming.
   mesh->EnsureNCMesh();

   // Define a parallel mesh by a partitioning of the serial mesh. Refine
   // this mesh further in parallel to increase the resolution. Once the
   // parallel mesh is defined, the serial mesh can be deleted.
   if ( mpi.Root() && logging > 0 )
   { cout << "Building Parallel Mesh ..." << endl; }
   ParMesh pmesh(MPI_COMM_WORLD, *mesh);
   delete mesh;

   if (mpi.Root())
   {
      cout << "Starting initialization." << endl;
   }
   /*
   double Bmag = BVec.Norml2();
   Vector BUnitVec(3);
   BUnitVec(0) = BVec(0)/Bmag;
   BUnitVec(1) = BVec(1)/Bmag;
   BUnitVec(2) = BVec(2)/Bmag;

   VectorConstantCoefficient BCoef(BVec);
   VectorConstantCoefficient BUnitCoef(BUnitVec);
   */
    
   H1_ParFESpace H1FESpace(&pmesh, order, pmesh.Dimension());
   ND_ParFESpace HCurlFESpace(&pmesh, order, pmesh.Dimension());
   RT_ParFESpace HDivFESpace(&pmesh, order, pmesh.Dimension());
   L2_ParFESpace L2FESpace(&pmesh, order, pmesh.Dimension());

   ParGridFunction BField(&HDivFESpace);
   ParGridFunction temperature_gf;
   ParGridFunction density_gf;
   ParGridFunction xposition_gf(&H1FESpace);
    
   PlasmaProfile xposCoef(xpt, xpp);
   xposition_gf.ProjectCoefficient(xposCoef);

   BFieldProfile BCoef(bpt, bpp, false);
   BField.ProjectCoefficient(BCoef);

   BFieldProfile BUnitCoef(bpt, bpp, true);

   int size_h1 = H1FESpace.GetVSize();
   int size_l2 = L2FESpace.GetVSize();

   Array<int> density_offsets(numbers.Size() + 1);
   Array<int> temperature_offsets(numbers.Size() + 2);

   density_offsets[0] = 0;
   temperature_offsets[0] = 0;
   temperature_offsets[1] = size_h1;

   for (int i=1; i<=numbers.Size(); i++)
   {
      density_offsets[i]     = density_offsets[i - 1] + size_l2;
      temperature_offsets[i + 1] = temperature_offsets[i] + size_h1;
   }

   BlockVector density(density_offsets);
   BlockVector temperature(temperature_offsets);

   if (mpi.Root())
   {
      cout << "Creating plasma profile." << endl;
   }

   PlasmaProfile tempCoef(tpt, tpp);
   PlasmaProfile rhoCoef(dpt, dpp);

   for (int i=0; i<=numbers.Size(); i++)
   {
      temperature_gf.MakeRef(&H1FESpace, temperature.GetBlock(i));
      temperature_gf.ProjectCoefficient(tempCoef);
   }
   for (int i=0; i<charges.Size(); i++)
   {
       density_gf.MakeRef(&L2FESpace, density.GetBlock(i));
       density_gf.ProjectCoefficient(rhoCoef);
       density_gf *= numbers[i]/numbers[0];
   }

   if (mpi.Root())
   {
      cout << "Creating coefficients for Maxwell equations." << endl;
   }

   // Create a coefficient describing the magnetic permeability
   ConstantCoefficient muInvCoef(1.0 / mu0_);

   // Create a coefficient describing the surface admittance
   Coefficient * etaInvCoef = SetupAdmittanceCoefficient(pmesh, abcs);

   // Create tensor coefficients describing the dielectric permittivity
   DielectricTensor epsilon_real(BField, xposition_gf, density, temperature,
                                            L2FESpace, H1FESpace,
                                            omega, charges, masses, nuprof,
                                            true);
   DielectricTensor epsilon_imag(BField, xposition_gf, density, temperature,
                                            L2FESpace, H1FESpace,
                                            omega, charges, masses, nuprof,
                                            false);
   SPDDielectricTensor epsilon_abs(BField, xposition_gf, density, temperature,
                                    L2FESpace, H1FESpace,
                                    omega, charges, masses, nuprof);
   SheathImpedance z_r(BField, density, temperature,
                        L2FESpace, H1FESpace,
                        omega, charges, masses, true);
   SheathImpedance z_i(BField, density, temperature,
                        L2FESpace, H1FESpace,
                        omega, charges, masses, false);
    
   MultiStrapAntennaH HReStrapCoef(msa_n, msa_p, true);
   MultiStrapAntennaH HImStrapCoef(msa_n, msa_p, false);

   /*
   ColdPlasmaPlaneWave EReCoef(wave_type[0], omega, BVec,
                               numbers, charges, masses, true);
   ColdPlasmaPlaneWave EImCoef(wave_type[0], omega, BVec,
                               numbers, charges, masses, false);
   */
   /*
   if (wave_type[0] == 'J' && slab_params_.Size() == 5)
   {
      EReCoef.SetCurrentSlab(slab_params_[1], slab_params_[3], slab_params_[4],
                             mesh_dim_[0]);
      EImCoef.SetCurrentSlab(slab_params_[1], slab_params_[3], slab_params_[4],
                             mesh_dim_[0]);
   }
   */
   /*
   if (phase_shift)
   {
      EReCoef.SetPhaseShift(kVec);
      EImCoef.SetPhaseShift(kVec);
   }
   */
    
    VectorConstantCoefficient kCoef(kVec);
    
    if (visualization && wave_type[0] != ' ')
   {
      if (mpi.Root())
      {
         cout << "Visualize input fields." << endl;
      }
      // ParComplexGridFunction EField(&HCurlFESpace);
      // EField.ProjectCoefficient(EReCoef, EImCoef);

      /*
      ParComplexGridFunction ZCoef(&H1FESpace);
      // Array<int> ess_bdr(mesh->bdr_attributes.Size());
      // ess_bdr = 1;
      // ZCoef.ProjectBdrCoefficient(z_r, z_i, ess_bdr);
      ZCoef.ProjectCoefficient(z_r, z_i);
       */

      char vishost[] = "localhost";
      int  visport   = 19916;

      int Wx = 0, Wy = 0; // window position
      int Ww = 350, Wh = 350; // window size
      int offx = Ww+10;//, offy = Wh+45; // window offsets

      socketstream /*sock_Er, sock_Ei, sock_zr, sock_zi, */ sock_B;
      // sock_Er.precision(8);
      // sock_Ei.precision(8);
      sock_B.precision(8);
      // sock_zr.precision(8);
      // sock_zi.precision(8);

      Wx += 2 * offx;
      /*
      VisualizeField(sock_Er, vishost, visport,
                     EField.real(), "Exact Electric Field, Re(E)",
                     Wx, Wy, Ww, Wh);
      Wx += offx;

      VisualizeField(sock_Ei, vishost, visport,
                     EField.imag(), "Exact Electric Field, Im(E)",
                     Wx, Wy, Ww, Wh);
      Wx -= offx;
      Wy += offy;
      */

      /*
      VisualizeField(sock_B, vishost, visport,
                    BField, "Background Magnetic Field",
                    Wx, Wy, Ww, Wh);


      VisualizeField(sock_zr, vishost, visport,
                    ZCoef.real(), "Real Sheath Impedance",
                    Wx, Wy, Ww, Wh);

      VisualizeField(sock_zi, vishost, visport,
                    ZCoef.imag(), "Imaginary Sheath Impedance",
                    Wx, Wy, Ww, Wh);
      */
      /*
      for (int i=0; i<charges.Size(); i++)
      {
         Wx += offx;

         socketstream sock;
         sock.precision(8);

         stringstream oss;
         oss << "Density Species " << i;
         density_gf.MakeRef(&L2FESpace, density.GetBlock(i));
         VisualizeField(sock, vishost, visport,
                        density_gf, oss.str().c_str(),
                        Wx, Wy, Ww, Wh);
      }


        socketstream sock;
        sock.precision(8);

        temperature_gf.MakeRef(&H1FESpace, temperature.GetBlock(0));
        VisualizeField(sock, vishost, visport,
                         temperature_gf, "Temp",
                         Wx, Wy, Ww, Wh);
       */
   }

   if (mpi.Root())
   {
      cout << "Setup boundary conditions." << endl;
   }
    
    // Setup coefficients for Dirichlet BC
   int dbcsSize = (peca.Size() > 0) + (dbca1.Size() > 0) + (dbca2.Size() > 0);

   Array<ComplexVectorCoefficientByAttr*> dbcs(dbcsSize);

   Vector zeroVec(3); zeroVec = 0.0;
   Vector dbc1ReVec;
   Vector dbc1ImVec;
   Vector dbc2ReVec;
   Vector dbc2ImVec;

    if (dbcv1.Size() >= 3)
    {
       dbc1ReVec.SetDataAndSize(&dbcv1[0], 3);
    }
    else
    {
       dbc1ReVec.SetDataAndSize(&zeroVec[0], 3);
    }
    if (dbcv1.Size() >= 6)
    {
       dbc1ImVec.SetDataAndSize(&dbcv1[3], 3);
    }
    else
    {
       dbc1ImVec.SetDataAndSize(&zeroVec[0], 3);
    }
    if (dbcv2.Size() >= 3)
    {
       dbc2ReVec.SetDataAndSize(&dbcv2[0], 3);
    }
    else
    {
       dbc2ReVec.SetDataAndSize(&zeroVec[0], 3);
    }
    if (dbcv2.Size() >= 6)
    {
       dbc2ImVec.SetDataAndSize(&dbcv2[3], 3);
    }
    else
    {
       dbc2ImVec.SetDataAndSize(&zeroVec[0], 3);
    }

    VectorConstantCoefficient zeroCoef(zeroVec);
    VectorConstantCoefficient dbc1ReCoef(dbc1ReVec);
    VectorConstantCoefficient dbc1ImCoef(dbc1ImVec);
    VectorConstantCoefficient dbc2ReCoef(dbc2ReVec);
    VectorConstantCoefficient dbc2ImCoef(dbc2ImVec);

    if (dbcsSize > 0)
    {
       int c = 0;
       if (peca.Size() > 0)
       {
          dbcs[c] = new ComplexVectorCoefficientByAttr;
          dbcs[c]->attr = peca;
          dbcs[c]->real = &zeroCoef;
          dbcs[c]->imag = &zeroCoef;
          c++;
       }
       if (dbca1.Size() > 0)
       {
          dbcs[c] = new ComplexVectorCoefficientByAttr;
          dbcs[c]->attr = dbca1;
          dbcs[c]->real = &dbc1ReCoef;
          dbcs[c]->imag = &dbc1ImCoef;
          c++;
       }
       if (dbca2.Size() > 0)
       {
          dbcs[c] = new ComplexVectorCoefficientByAttr;
          dbcs[c]->attr = dbca2;
          dbcs[c]->real = &dbc2ReCoef;
          dbcs[c]->imag = &dbc2ImCoef;
          c++;
       }
    }

    int nbcsSize = (nbca1.Size() > 0) + (nbca2.Size() > 0) + (nbcas.Size() > 0);

    Array<ComplexVectorCoefficientByAttr*> nbcs(nbcsSize);

    Vector nbc1ReVec;
    Vector nbc1ImVec;
    Vector nbc2ReVec;
    Vector nbc2ImVec;

    if (nbcv1.Size() >= 3)
    {
       nbc1ReVec.SetDataAndSize(&nbcv1[0], 3);
    }
    else
    {
       nbc1ReVec.SetDataAndSize(&zeroVec[0], 3);
    }
    if (nbcv1.Size() >= 6)
    {
       nbc1ImVec.SetDataAndSize(&nbcv1[3], 3);
    }
    else
    {
       nbc1ImVec.SetDataAndSize(&zeroVec[0], 3);
    }
    if (nbcv2.Size() >= 3)
    {
       nbc2ReVec.SetDataAndSize(&nbcv2[0], 3);
    }
    else
    {
       nbc2ReVec.SetDataAndSize(&zeroVec[0], 3);
    }
    if (nbcv2.Size() >= 6)
    {
       nbc2ImVec.SetDataAndSize(&nbcv2[3], 3);
    }
    else
    {
       nbc2ImVec.SetDataAndSize(&zeroVec[0], 3);
    }

    VectorConstantCoefficient nbc1ReCoef(nbc1ReVec);
    VectorConstantCoefficient nbc1ImCoef(nbc1ImVec);
    VectorConstantCoefficient nbc2ReCoef(nbc2ReVec);
    VectorConstantCoefficient nbc2ImCoef(nbc2ImVec);

    if (nbcsSize > 0)
    {
       int c = 0;
       if (nbca1.Size() > 0)
       {
          nbcs[c] = new ComplexVectorCoefficientByAttr;
          nbcs[c]->attr = nbca1;
          nbcs[c]->real = &nbc1ReCoef;
          nbcs[c]->imag = &nbc1ImCoef;
          c++;
       }
       if (nbca2.Size() > 0)
       {
          nbcs[c] = new ComplexVectorCoefficientByAttr;
          nbcs[c]->attr = nbca2;
          nbcs[c]->real = &nbc2ReCoef;
          nbcs[c]->imag = &nbc2ImCoef;
          c++;
       }
       if (nbcas.Size() > 0)
       {
          nbcs[c] = new ComplexVectorCoefficientByAttr;
          nbcs[c]->attr = nbcas;
          nbcs[c]->real = &HReStrapCoef;
          nbcs[c]->imag = &HImStrapCoef;
          c++;
       }
    }

    Array<ComplexCoefficientByAttr*> sbcs((sbca.Size() > 0)? 1 : 0);
    if (sbca.Size() > 0)
    {
       sbcs[0] = new ComplexCoefficientByAttr;
       sbcs[0]->real = &z_r;
       sbcs[0]->imag = &z_i;
       sbcs[0]->attr = sbca;
       AttrToMarker(pmesh.bdr_attributes.Max(), sbcs[0]->attr,
                    sbcs[0]->attr_marker);
    }


   if (mpi.Root())
   {
      cout << "Creating Cold Plasma Dielectric solver." << endl;
   }

   // Create the cold plasma EM solver
   CPDSolver CPD(pmesh, order, omega,
                 (CPDSolver::SolverType)sol, solOpts,
                 (CPDSolver::PrecondType)prec,
                 conv, BUnitCoef,
                 epsilon_real, epsilon_imag, epsilon_abs,
                 muInvCoef, etaInvCoef,
                 (phase_shift) ? &kCoef : NULL,
                 //(phase_shift) ? &kReCoef : NULL,
                 //(phase_shift) ? &kImCoef : NULL,
                 abcs,
                 dbcs, nbcs, sbcs,
                 // e_bc_r, e_bc_i,
                 // EReCoef, EImCoef,
                 (rod_params_.Size() > 0 ||slab_params_.Size() > 0) ?
                 j_src_r : NULL,
                 (rod_params_.Size() > 0 ||slab_params_.Size() > 0) ?
                 j_src_i : NULL, vis_u, pa);

   // Initialize GLVis visualization
   if (visualization)
   {
      CPD.InitializeGLVis();
   }

   // Initialize VisIt visualization
   VisItDataCollection visit_dc("STIX2D-AMR-Parallel", &pmesh);

   if ( visit )
   {
      CPD.RegisterVisItFields(visit_dc);
      temperature_gf.MakeRef(&H1FESpace, temperature.GetBlock(0));
      visit_dc.RegisterField("Electron_Temp", &temperature_gf);

      density_gf.MakeRef(&L2FESpace, density.GetBlock(0));
      visit_dc.RegisterField("Electron_Density", &density_gf);
   }
   if (mpi.Root()) { cout << "Initialization done." << endl; }

   // The main AMR loop. In each iteration we solve the problem on the current
   // mesh, visualize the solution, estimate the error on all elements, refine
   // the worst elements and update all objects to work with the new mesh. We
   // refine until the maximum number of dofs in the Nedelec finite element
   // space reaches 10 million.
   const int max_dofs = 10000000;
   for (int it = 1; it <= maxit; it++)
   {
      if (mpi.Root())
      {
         cout << "\nAMR Iteration " << it << endl;
      }

      // Display the current number of DoFs in each finite element space
      CPD.PrintSizes();

      // Assemble all forms
      CPD.Assemble();

      // Solve the system and compute any auxiliary fields
      CPD.Solve();

      /*
      // Compute error
      double glb_error = CPD.GetError(EReCoef, EImCoef);
      if (mpi.Root())
      {
         cout << "Global L2 Error " << glb_error << endl;
      }
      */

      // Determine the current size of the linear system
      int prob_size = CPD.GetProblemSize();

      // Write fields to disk for VisIt
      if ( visit )
      {
         CPD.WriteVisItFields(it);
      }

      // Send the solution by socket to a GLVis server.
      if (visualization)
      {
         CPD.DisplayToGLVis();
      }

      if (mpi.Root())
      {
         cout << "AMR iteration " << it << " complete." << endl;
      }

      // Check stopping criteria
      if (prob_size > max_dofs)
      {
         if (mpi.Root())
         {
            cout << "Reached maximum number of dofs, exiting..." << endl;
         }
         break;
      }
      if ( it == maxit )
      {
         break;
      }

      // Wait for user input. Ask every 10th iteration.
      char c = 'c';
      if (mpi.Root() && (it % 10 == 0))
      {
         cout << "press (q)uit or (c)ontinue --> " << flush;
         cin >> c;
      }
      MPI_Bcast(&c, 1, MPI_CHAR, 0, MPI_COMM_WORLD);

      if (c != 'c')
      {
         break;
      }

      // Estimate element errors using the Zienkiewicz-Zhu error estimator.
      Vector errors(pmesh.GetNE());
      CPD.GetErrorEstimates(errors);

      double local_max_err = errors.Max();
      double global_max_err;
      MPI_Allreduce(&local_max_err, &global_max_err, 1,
                    MPI_DOUBLE, MPI_MAX, pmesh.GetComm());

      // Refine the elements whose error is larger than a fraction of the
      // maximum element error.
      const double frac = 0.5;
      double threshold = frac * global_max_err;
      if (mpi.Root()) { cout << "Refining ..." << endl; }
      {
         pmesh.RefineByError(errors, threshold);
         /*
              Array<Refinement> refs;
              for (int i=0; i<pmesh.GetNE(); i++)
              {
                 if (errors[i] > threshold)
                 {
                    refs.Append(Refinement(i, 3));
                 }
              }
              if (refs.Size() > 0)
              {
                 pmesh.GeneralRefinement(refs);
              }
         */
      }

      // Update the magnetostatic solver to reflect the new state of the mesh.
      Update(H1FESpace, HCurlFESpace, HDivFESpace, L2FESpace, BField, BCoef,
             rhoCoef, tempCoef, xposCoef,
             size_h1, size_l2,
             density_offsets, temperature_offsets,
             density, temperature,
             density_gf, temperature_gf, xposition_gf);
      CPD.Update();

      if (pmesh.Nonconforming() && mpi.WorldSize() > 1 && false)
      {
         if (mpi.Root()) { cout << "Rebalancing ..." << endl; }
         pmesh.Rebalance();

         // Update again after rebalancing
         Update(H1FESpace, HCurlFESpace, HDivFESpace, L2FESpace, BField, BCoef,
                rhoCoef, tempCoef, xposCoef,
                size_h1, size_l2,
                density_offsets, temperature_offsets,
                density, temperature,
                density_gf, temperature_gf, xposition_gf);
         CPD.Update();
      }
   }

   // Send the solution by socket to a GLVis server.
   if (visualization)
   {
      CPD.DisplayAnimationToGLVis();
   }

   // delete epsCoef;
   // delete muInvCoef;
   // delete sigmaCoef;

   return 0;
}

void Update(ParFiniteElementSpace & H1FESpace,
            ParFiniteElementSpace & HCurlFESpace,
            ParFiniteElementSpace & HDivFESpace,
            ParFiniteElementSpace & L2FESpace,
            ParGridFunction & BField,
            VectorCoefficient & BCoef,
            Coefficient & rhoCoef,
            Coefficient & TCoef,
            Coefficient & xposCoef,
            int & size_h1,
            int & size_l2,
            Array<int> & density_offsets,
            Array<int> & temperature_offsets,
            BlockVector & density,
            BlockVector & temperature,
            ParGridFunction & density_gf,
            ParGridFunction & temperature_gf,
            ParGridFunction & xposition_gf)
{
   H1FESpace.Update();
   HCurlFESpace.Update();
   HDivFESpace.Update();
   L2FESpace.Update();

   BField.Update();
   BField.ProjectCoefficient(BCoef);
    
   xposition_gf.Update();
   xposition_gf.ProjectCoefficient(xposCoef);

   size_l2 = L2FESpace.GetVSize();
   for (int i=1; i<density_offsets.Size(); i++)
   {
      density_offsets[i] = density_offsets[i - 1] + size_l2;
   }
   density.Update(density_offsets);
   for (int i=0; i<density_offsets.Size()-1; i++)
   {
      density_gf.MakeRef(&L2FESpace, density.GetBlock(i));
      density_gf.ProjectCoefficient(rhoCoef);
   }

   size_h1 = H1FESpace.GetVSize();
   for (int i=1; i<temperature_offsets.Size(); i++)
   {
      temperature_offsets[i] = temperature_offsets[i - 1] + size_h1;
   }
   temperature.Update(temperature_offsets);
   for (int i=0; i<temperature_offsets.Size()-1; i++)
   {
      temperature_gf.MakeRef(&H1FESpace, temperature.GetBlock(i));
      temperature_gf.ProjectCoefficient(TCoef);
   }
}

// Print the stix2d ascii logo to the given ostream
void display_banner(ostream & os)
{
   os << "  _________ __   __       ________      ___" << endl
      << " /   _____//  |_|__|__  __\\_____  \\  __| _/" << endl
      << " \\_____  \\\\   __\\  \\  \\/  //  ____/ / __ | " << endl
      << " /        \\|  | |  |>    </       \\/ /_/ | " << endl
      << "/_______  /|__| |__/__/\\_ \\_______ \\____ | " << endl
      << "        \\/               \\/       \\/    \\/ "  << endl
      << endl
      << "* Thomas H. Stix was a pioneer in the use of radio frequency"
      << " waves to heat" << endl
      << "  terrestrial plasmas to solar temperatures. He made important"
      << " contributions" << endl
      << "  to experimental and theoretic plasma physics. In the Stix"
      << " application, the" << endl
      << "  plasma dielectric for the wave equation is formulated using"
      << " the \"Stix\"" << endl
      << "  notation, \"S, D, P\"." << endl<< endl << flush;
}

// The Admittance is an optional coefficient defined on boundary surfaces which
// can be used in conjunction with absorbing boundary conditions.
Coefficient *
SetupAdmittanceCoefficient(const Mesh & mesh, const Array<int> & abcs)
{
   Coefficient * coef = NULL;

   if ( pw_eta_.Size() > 0 )
   {
      MFEM_VERIFY(pw_eta_.Size() == abcs.Size(),
                  "Each impedance value must be associated with exactly one "
                  "absorbing boundary surface.");

      pw_eta_inv_.SetSize(mesh.bdr_attributes.Size());

      if ( abcs[0] == -1 )
      {
         pw_eta_inv_ = 1.0 / pw_eta_[0];
      }
      else
      {
         pw_eta_inv_ = 0.0;

         for (int i=0; i<pw_eta_.Size(); i++)
         {
            pw_eta_inv_[abcs[i]-1] = 1.0 / pw_eta_[i];
         }
      }
      coef = new PWConstCoefficient(pw_eta_inv_);
   }

   return coef;
}

void rod_current_source_r(const Vector &x, Vector &j)
{
   MFEM_ASSERT(x.Size() == 3, "current source requires 3D space.");

   j.SetSize(x.Size());
   j = 0.0;

   bool cmplx = rod_params_.Size() == 9;

   int o = 3 + (cmplx ? 3 : 0);

   double x0 = rod_params_(o+0);
   double y0 = rod_params_(o+1);
   double radius = rod_params_(o+2);

   double r2 = (x(0) - x0) * (x(0) - x0) + (x(1) - y0) * (x(1) - y0);

   if (r2 <= radius * radius)
   {
      j(0) = rod_params_(0);
      j(1) = rod_params_(1);
      j(2) = rod_params_(2);
   }
   // j *= height;
}

void rod_current_source_i(const Vector &x, Vector &j)
{
   MFEM_ASSERT(x.Size() == 3, "current source requires 3D space.");

   j.SetSize(x.Size());
   j = 0.0;

   bool cmplx = rod_params_.Size() == 9;

   int o = 3 + (cmplx ? 3 : 0);

   double x0 = rod_params_(o+0);
   double y0 = rod_params_(o+1);
   double radius = rod_params_(o+2);

   double r2 = (x(0) - x0) * (x(0) - x0) + (x(1) - y0) * (x(1) - y0);

   if (r2 <= radius * radius)
   {
      if (cmplx)
      {
         j(0) = rod_params_(3);
         j(1) = rod_params_(4);
         j(2) = rod_params_(5);
      }
   }
   // j *= height;
}

void slab_current_source_r(const Vector &x, Vector &j)
{
   MFEM_ASSERT(x.Size() == 3, "current source requires 3D space.");

   j.SetSize(x.Size());
   j = 0.0;

   bool cmplx = slab_params_.Size() == 10;

   int o = 3 + (cmplx ? 3 : 0);

   double x0 = slab_params_(o+0);
   double y0 = slab_params_(o+1);
   double dx = slab_params_(o+2);
   double dy = slab_params_(o+3);

   if (x[0] >= x0-0.5*dx && x[0] <= x0+0.5*dx &&
       x[1] >= y0-0.5*dy && x[1] <= y0+0.5*dy)
   {
      j(0) = slab_params_(0);
      j(1) = slab_params_(1);
      j(2) = slab_params_(2);
      if (slab_profile_ == 1)
      { j *= 0.5 * (1.0 + sin(M_PI*((2.0 * (x[1] - y0) + dy)/dy - 0.5))); }
   }
}

void slab_current_source_i(const Vector &x, Vector &j)
{
   MFEM_ASSERT(x.Size() == 3, "current source requires 3D space.");

   j.SetSize(x.Size());
   j = 0.0;

   bool cmplx = slab_params_.Size() == 10;

   int o = 3 + (cmplx ? 3 : 0);

   double x0 = slab_params_(o+0);
   double y0 = slab_params_(o+1);
   double dx = slab_params_(o+2);
   double dy = slab_params_(o+3);

   if (x[0] >= x0-0.5*dx && x[0] <= x0+0.5*dx &&
       x[1] >= y0-0.5*dy && x[1] <= y0+0.5*dy)
   {
      if (cmplx)
      {
         j(0) = slab_params_(3);
         j(1) = slab_params_(4);
         j(2) = slab_params_(5);
         if (slab_profile_ == 1)
         { j *= 0.5 * (1.0 + sin(M_PI*((2.0 * (x[1] - y0) + dy)/dy - 0.5))); }
      }
   }
}
void e_bc_r(const Vector &x, Vector &E)
{
   E.SetSize(3);
   E = 0.0;

}

void e_bc_i(const Vector &x, Vector &E)
{
   E.SetSize(3);
   E = 0.0;
}
/*
ColdPlasmaPlaneWave::ColdPlasmaPlaneWave(char type,
                                         double omega,
                                         const Vector & B,
                                         const Vector & number,
                                         const Vector & charge,
                                         const Vector & mass,
                                         bool realPart)
   : VectorCoefficient(3),
     type_(type),
     realPart_(realPart),
     omega_(omega),
     Bmag_(B.Norml2()),
     Jy_(0.0),
     xJ_(0.5),
     Lx_(1.0),
     k_(0),
     B_(B),
     numbers_(number),
     charges_(charge),
     masses_(mass)
{
   S_ = S_cold_plasma(omega_, Bmag_, numbers_, charges_, masses_);
   D_ = D_cold_plasma(omega_, Bmag_, numbers_, charges_, masses_);
   P_ = P_cold_plasma(omega_, numbers_, charges_, masses_);
}

void ColdPlasmaPlaneWave::Eval(Vector &V, ElementTransformation &T,
                               const IntegrationPoint &ip)
{
   V.SetSize(3);

   double x_data[3];
   Vector x(x_data, 3);
   T.Transform(ip, x);

   switch (type_)
   {
      case 'L':
      {
         bool osc = S_ - D_ > 0.0;
         double kL = omega_ * sqrt(fabs(S_-D_)) / c0_;

         if (realPart_)
         {
            V[0] = 0.0;
            V[1] = osc ?  sin(kL * x[0]) : 0.0;
            V[2] = osc ?  cos(kL * x[0]) : exp(-kL * x[0]);
         }
         else
         {
            V[0] = 0.0;
            V[1] = osc ?  cos(kL * x[0]) : exp(-kL * x[0]);
            V[2] = osc ? -sin(kL * x[0]) : 0.0;
         }
      }
      break;
      case 'R':
      {
         bool osc = S_ + D_ > 0.0;
         double kR = omega_ * sqrt(fabs(S_+D_)) / c0_;

         if (realPart_)
         {
            V[0] = 0.0;
            V[1] = osc ? -sin(kR * x[0]) : 0.0;
            V[2] = osc ?  cos(kR * x[0]) : exp(-kR * x[0]);
         }
         else
         {
            V[0] = 0.0;
            V[1] = osc ? -cos(kR * x[0]) : -exp(-kR * x[0]);
            V[2] = osc ? -sin(kR * x[0]) : 0.0;
         }
      }
      break;
      case 'O':
      {
         bool osc = P_ > 0.0;
         double kO = omega_ * sqrt(fabs(P_)) / c0_;

         if (realPart_)
         {
            V[0] = 0.0;
            V[1] = osc ? cos(kO * x[0]) : exp(-kO * x[0]);
            V[2] = 0.0;
         }
         else
         {
            V[0] = 0.0;
            V[1] = osc ? -sin(kO * x[0]) : 0.0;
            V[2] = 0.0;
         }
      }
      break;
      case 'X':
      {
         bool osc = (S_ * S_ - D_ * D_) / S_ > 0.0;
         double kE = omega_ * sqrt(fabs((S_ * S_ - D_ * D_) / S_)) / c0_;

         if (realPart_)
         {
            V[0] = osc ? -D_ * sin(kE * x[0]) : 0.0;
            V[1] = 0.0;
            V[2] = osc ?  S_ * cos(kE * x[0]) : S_ * exp(-kE * x[0]);
         }
         else
         {
            V[0] = osc ? -D_ * cos(kE * x[0]) : -D_ * exp(-kE * x[0]);
            V[1] = 0.0;
            V[2] = osc ? -S_ * sin(kE * x[0]) : 0.0;
         }
         V /= sqrt(S_ * S_ + D_ * D_);
      }
      break;
      case 'J':
      {
         if (k_.Size() == 0)
         {
            bool osc = (S_ * S_ - D_ * D_) / S_ > 0.0;
            double kE = omega_ * sqrt(fabs((S_ * S_ - D_ * D_) / S_)) / c0_;

            double (*sfunc)(double) = osc ?
                                      static_cast<double (*)(double)>(&sin) :
                                      static_cast<double (*)(double)>(&sinh);
            double (*cfunc)(double) = osc ?
                                      static_cast<double (*)(double)>(&cos) :
                                      static_cast<double (*)(double)>(&cosh);

            double skL   = (*sfunc)(kE * Lx_);
            double csckL = 1.0 / skL;

            if (realPart_)
            {
               V[0] = D_ / S_;
               V[1] = 0.0;
               V[2] = 0.0;
            }
            else
            {
               V[0] = 0.0;
               V[1] = -1.0;
               V[2] = 0.0;
            }

            if (x[0] <= xJ_ - 0.5 * dx_)
            {
               double skx    = (*sfunc)(kE * x[0]);
               double skLxJ  = (*sfunc)(kE * (Lx_ - xJ_));
               double skd    = (*sfunc)(kE * 0.5 * dx_);
               double a = skx * skLxJ * skd;

               V *= 2.0 * omega_ * mu0_ * Jy_ * a * csckL / (kE * kE);
               if (!osc) { V *= -1.0; }
            }
            else if (x[0] <= xJ_ + 0.5 * dx_)
            {
               double skx      = (*sfunc)(kE * x[0]);
               double skLx     = (*sfunc)(kE * (Lx_ - x[0]));
               double ckxJmd   = (*cfunc)(kE * (xJ_ - 0.5 * dx_));
               double ckLxJmd  = (*cfunc)(kE * (Lx_ - xJ_ - 0.5 * dx_));
               double a = skx * ckLxJmd + skLx * ckxJmd - skL;

               V *= omega_ * mu0_ * Jy_ * a * csckL / (kE * kE);
            }
            else
            {
               double skLx = (*sfunc)(kE * (Lx_ - x[0]));
               double skxJ = (*sfunc)(kE * xJ_);
               double skd  = (*sfunc)(kE * 0.5 * dx_);
               double a = skLx * skxJ * skd;

               V *= 2.0 * omega_ * mu0_ * Jy_ * a * csckL / (kE * kE);
               if (!osc) { V *= -1.0; }
            }
         }
         else
         {
            // General phase shift
            V = 0.0; // For now...
         }
      }
      break;
      case 'Z':
         V = 0.0;
         break;
   }
}
*/
