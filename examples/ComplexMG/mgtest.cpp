//                       MFEM Example 0 - Parallel Version
//
// Compile with: make ex0p
//
// Sample runs:  mpirun -np 4 ex0p
//               mpirun -np 4 ex0p -m ../data/fichera.mesh
//               mpirun -np 4 ex0p -m ../data/square-disc.mesh -o 2
//
// Description: This example code demonstrates the most basic parallel usage of
//              MFEM to define a simple finite element discretization of the
//              Laplace problem -Delta u = 1 with zero Dirichlet boundary
//              conditions. General 2D/3D serial mesh files and finite element
//              polynomial degrees can be specified by command line options.

#include "mfem.hpp"
#include <fstream>
#include <iostream>
#include "mg.hpp"

using namespace std;
using namespace mfem;

int main(int argc, char *argv[])
{
   // 1. Initialize MPI
   MPI_Session mpi(argc, argv);

   // 2. Parse command line options
   const char *mesh_file = "../data/star.mesh";
   int order = 1;
   int href = 1;
   int pref = 0;

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh", "Mesh file to use.");
   args.AddOption(&order, "-o", "--order", "Finite element polynomial degree");
   args.AddOption(&href, "-gr", "--geometric-ref", "Number of Geometric refinements");
   args.AddOption(&pref, "-or", "--order-ref", "Number of Order refinements");
   args.ParseCheck();

   // 3. Read the serial mesh from the given mesh file.
   Mesh mesh(mesh_file);

   // 4. Define a parallel mesh by a partitioning of the serial mesh. Refine
   //    this mesh once in parallel to increase the resolution.
   ParMesh pmesh(MPI_COMM_WORLD, mesh);
   mesh.Clear(); // the serial mesh is no longer needed
   pmesh.UniformRefinement();

   // 5. Define a finite element space on the mesh. Here we use H1 continuous
   //    high-order Lagrange finite elements of the given order.
   H1_FECollection fec(order, pmesh.Dimension());
   ParFiniteElementSpace fespace(&pmesh, &fec);


   std::vector<ParFiniteElementSpace * > fespaces(href+1);
   std::vector<ParMesh * > ParMeshes(href+1);
   std::vector<HypreParMatrix*>  P(href);
   for (int i = 0; i < href; i++)
   {
      ParMeshes[i] = new ParMesh(pmesh);
      fespaces[i]  = new ParFiniteElementSpace(fespace, *ParMeshes[i]);
      pmesh.UniformRefinement();
      // Update fespace
      fespace.Update();
      OperatorHandle Tr(Operator::Hypre_ParCSR);
      fespace.GetTrueTransferOperator(*fespaces[i], Tr);
      Tr.SetOperatorOwner(false);
      Tr.Get(P[i]);
   }
   fespaces[href] = new ParFiniteElementSpace(fespace);


   HYPRE_BigInt total_num_dofs = fespace.GlobalTrueVSize();
   if (mpi.Root()) { cout << "Number of unknowns: " << total_num_dofs << endl; }

   // 6. Extract the list of all the boundary DOFs. These will be marked as
   //    Dirichlet in order to enforce zero boundary conditions.
   Array<int> boundary_dofs;
   fespace.GetBoundaryTrueDofs(boundary_dofs);

   // 7. Define the solution x as a finite element grid function in fespace. Set
   //    the initial guess to zero, which also sets the boundary conditions.
   ParGridFunction x(&fespace);
   x = 0.0;

   // 8. Set up the linear form b(.) corresponding to the right-hand side.
   ConstantCoefficient one(1.0);
   ParLinearForm b(&fespace);
   b.AddDomainIntegrator(new DomainLFIntegrator(one));
   b.Assemble();

   // 9. Set up the bilinear form a(.,.) corresponding to the -Delta operator.
   ParBilinearForm a(&fespace);
   a.AddDomainIntegrator(new DiffusionIntegrator);
   a.Assemble();

   // 10. Form the linear system A X = B. This includes eliminating boundary
   //     conditions, applying AMR constraints, parallel assembly, etc.
   HypreParMatrix A;
   Vector B, X;
   a.FormLinearSystem(boundary_dofs, x, b, A, X, B);

   // 11. Solve the system using PCG with hypre's BoomerAMG preconditioner.
   // HypreBoomerAMG M(A);
   MGSolver * M = new MGSolver(&A,P,fespaces);
   M->SetTheta(0.33);

   // SchwarzSmoother * M = new SchwarzSmoother(fespace.GetParMesh(), 0, &fespace, &A);
   // M->SetDumpingParam(0.3);
   // M->SetNumSmoothSteps(1);
   // M->SetTheta(0.33);
   CGSolver cg(MPI_COMM_WORLD);
   cg.SetRelTol(1e-12);
   cg.SetMaxIter(2000);
   cg.SetPrintLevel(1);
   cg.SetPreconditioner(*M);
   cg.SetOperator(A);
   cg.Mult(B, X);

   // 12. Recover the solution x as a grid function and save to file. The output
   //     can be viewed using GLVis as follows: "glvis -np <np> -m mesh -g sol"
   a.RecoverFEMSolution(X, b, x);

   char vishost[] = "localhost";
   int  visport   = 19916;
   socketstream sol_sock(vishost, visport);
   sol_sock << "parallel " << mpi.WorldSize() << " " << mpi.WorldRank() << "\n";
   sol_sock.precision(8);
   sol_sock << "solution\n" << pmesh << x << flush;


   return 0;
}