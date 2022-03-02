//                                MFEM modified from Example 10 and 16
//
// Compile with: make imAMRMHDp
//
// Sample runs:
// mpirun -n 4 imAMRMHDp -m Meshes/xperiodic-new.mesh -rs 4 -rp 0 -o 3 -i 3 -tf 1 -dt .1 -usepetsc --petscopts petscrc/rc_debug -s 3 -shell -amrl 3 -ltol 1e-3 -derefine
//
// Description:  this function only supports amr and implicit solvers
// Author: QT

#include "mfem.hpp"
#include "myCoefficient.hpp"
#include "myIntegrator.hpp"
#include "imResistiveMHDOperatorp.hpp"
#include "AMRResistiveMHDOperatorp.hpp"
#include "BlockZZEstimator.hpp"
#include "PCSolver.hpp"
#include "InitialConditions.hpp"
#include "../navier/ortho_solver.hpp"
#include <memory>
#include <iostream>
#include <fstream>

#ifndef MFEM_USE_PETSC
#error This example requires that MFEM is built with MFEM_USE_PETSC=YES
#endif
double beta;
double Lx;  
double lambda;
double resiG;
double ep=.2;
int icase = 1;
ParMesh *pmesh;

//this is an AMR update function for VSize (instead of TrueVSize)
//It is only called in the initial stage of AMR to generate an adaptive mesh
void AMRUpdate(BlockVector &S, BlockVector &S_tmp,
               Array<int> &offset,
               ParGridFunction &phi,
               ParGridFunction &psi,
               ParGridFunction &w,
               ParGridFunction &j)
{
   ParFiniteElementSpace* H1FESpace = phi.ParFESpace();

   //update fem space
   H1FESpace->Update();

   int fe_size = H1FESpace->GetVSize();

   //update offset vector
   offset[0] = 0;
   offset[1] = fe_size;
   offset[2] = 2*fe_size;
   offset[3] = 3*fe_size;
   offset[4] = 4*fe_size;

   S_tmp = S;
   S.Update(offset);
    
   const Operator* H1Update = H1FESpace->GetUpdateOperator();

   H1Update->Mult(S_tmp.GetBlock(0), S.GetBlock(0));
   H1Update->Mult(S_tmp.GetBlock(1), S.GetBlock(1));
   H1Update->Mult(S_tmp.GetBlock(2), S.GetBlock(2));
   H1Update->Mult(S_tmp.GetBlock(3), S.GetBlock(3));

   phi.MakeRef(H1FESpace, S, offset[0]);
   psi.MakeRef(H1FESpace, S, offset[1]);
     w.MakeRef(H1FESpace, S, offset[2]);
     j.MakeRef(H1FESpace, S, offset[3]);

   S_tmp.Update(offset);
   H1FESpace->UpdatesFinished();
}

//this is an update function for block vector of TureVSize
void AMRUpdateTrue(BlockVector &S, 
               Array<int> &true_offset,
               ParGridFunction &phi,
               ParGridFunction &psi,
               ParGridFunction &w,
               ParGridFunction &j,
               ParGridFunction *pre)
{
   FiniteElementSpace* H1FESpace = phi.FESpace();

   //++++Update the GridFunctions so that they match S
   phi.SetFromTrueDofs(S.GetBlock(0));
   psi.SetFromTrueDofs(S.GetBlock(1));
   w.SetFromTrueDofs(S.GetBlock(2));

   //update fem space
   H1FESpace->Update();

   // Compute new dofs on the new mesh
   phi.Update();
   psi.Update();
   w.Update();
   
   // Note j stores data as a regular gridfunction
   j.Update();
   if (pre!=NULL) pre->Update();

   int fe_size = H1FESpace->GetTrueVSize();

   //update offset vector
   true_offset[0] = 0;
   true_offset[1] = fe_size;
   true_offset[2] = 2*fe_size;
   true_offset[3] = 3*fe_size;

   // Resize S
   S.Update(true_offset);

   // Compute "true" dofs and store them in S
   phi.GetTrueDofs(S.GetBlock(0));
   psi.GetTrueDofs(S.GetBlock(1));
     w.GetTrueDofs(S.GetBlock(2));

   H1FESpace->UpdatesFinished();
}

int main(int argc, char *argv[])
{
   int num_procs, myid, myid_rand;
   MPI_Init(&argc, &argv);
   MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);

   myid_rand=rand();

   //----Parse command-line options----
   const char *mesh_file = "./Meshes/xperiodic-square.mesh";
   int ser_ref_levels = 2;
   int par_ref_levels = 0;
   int order = 2;
   int ode_solver_type = 2;
   double t_final = 5.0;
   double t_change = 0.;
   double dt = 0.0001;
   double visc = 1e-3;
   double resi = 1e-3;
   bool visit = false;
   bool paraview = false;
   bool use_petsc = false;
   bool use_factory = false;
   bool useStab = false; //use a stabilized formulation (explicit case only)
   bool initial_refine = false;
   bool yRange = false; //fix a refinement region along y direction
   bool compute_pressure = false;
   const char *petscrc_file = "";

   //----amr coefficients----
   int amr_levels=0;
   double ltol_amr=1e-5;
   bool derefine = false;
   int precision = 8;
   int nc_limit = 1;         // maximum level of hanging nodes
   int ref_steps=4;
   int iestimator=1;
   double err_ratio=.1;
   double err_fraction=.5;
   double derefine_ratio=.2;
   double derefine_fraction=.05;
   int ref_its=1;
   int deref_its=1;
   double t_refs=1e10;
   int    t_refs_steps=2;
   double error_norm=infinity();
   //----end of amr----
   
   //----problem paramters----
   beta = 0.001; 
   Lx=3.0;
   lambda=5.0;

   bool saveOne=false;
   bool checkpt=false;
   bool visualization = true;
   int vis_steps = 10;

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&ser_ref_levels, "-rs", "--refine",
                  "Number of times to refine the mesh uniformly in serial.");
   args.AddOption(&par_ref_levels, "-rp", "--refineP",
                  "Number of times to refine the mesh uniformly in parallel.");
   args.AddOption(&amr_levels, "-amrl", "--amr-levels",
                  "AMR refine level.");
   args.AddOption(&order, "-o", "--order",
                  "Order (degree) of the finite elements.");
   args.AddOption(&ode_solver_type, "-s", "--ode-solver",
                  "ODE solver: 1 - Backward Euler, 3 - L-stable SDIRK23, 4 - L-stable SDIRK33,\n\t"
                  "            22 - Implicit Midpoint, 23 - SDIRK23, 24 - SDIRK34.");
   args.AddOption(&t_final, "-tf", "--t-final",
                  "Final time; start time is 0.");
   args.AddOption(&t_change, "-tchange", "--t-change",
                  "dt change time; reduce to half.");
   args.AddOption(&t_refs, "-t-refs", "--t-refs",
                  "Time a quick refine/derefine is turned on.");
   args.AddOption(&t_refs_steps, "-t-refs-steps", "--t-refs-steps",
                  "Refine steps for a quick refine/derefine after t_refs.");
   args.AddOption(&dt, "-dt", "--time-step",
                  "Time step.");
   args.AddOption(&icase, "-i", "--icase",
                  "Icase: 1 - wave propagation; 2 - Tearing mode.");
   args.AddOption(&ijacobi, "-ijacobi", "--ijacobi",
                  "Number of jacobi iteration in preconditioner");
   args.AddOption(&im_supg, "-im_supg", "--im_supg",
                  "supg options in formulation");
   args.AddOption(&i_supgpre, "-i_supgpre", "--i_supgpre",
                  "supg preconditioner options in formulation");
   args.AddOption(&ex_supg, "-ex_supg", "--ex_supg",
                  "supg options in explicit formulation");
   args.AddOption(&itau_, "-itau", "--itau",
                  "tau options in supg.");
   args.AddOption(&visc, "-visc", "--viscosity",
                  "Viscosity coefficient.");
   args.AddOption(&resi, "-resi", "--resistivity",
                  "Resistivity coefficient.");
   args.AddOption(&ALPHA, "-alpha", "--hyperdiff",
                  "Numerical hyprediffusion coefficient.");
   args.AddOption(&beta, "-beta", "--perturb",
                  "Pertubation coefficient in initial conditions.");
   args.AddOption(&ltol_amr, "-ltol", "--local-tol",
                  "Local AMR tolerance.");
   args.AddOption(&err_ratio, "-err-ratio", "--err-ratio",
                  "AMR component ratio.");
   args.AddOption(&err_fraction, "-err-fraction", "--err-fraction",
                  "AMR error fraction in estimator.");
   args.AddOption(&derefine_ratio, "-derefine-ratio", "--derefine-ratio",
                  "AMR derefine error ratio.");
   args.AddOption(&derefine_fraction, "-derefine-fraction", "--derefine-fraction",
                  "AMR derefine error fraction of total error (derefine if error is less than portion of total error).");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&ref_steps, "-refs", "--refine-steps",
                  "Refine or derefine every n-th timestep.");
   args.AddOption(&vis_steps, "-vs", "--visualization-steps",
                  "Visualize every n-th timestep.");
   args.AddOption(&usesupg, "-supg", "--implicit-supg", "-no-supg",
                  "--no-implicit-supg",
                  "Use supg in the implicit solvers.");
   args.AddOption(&useStab, "-stab", "--explicit-stab", "-no-stab","--no-explitcit-stab",
                  "Use supg in the explicit solvers.");
   args.AddOption(&maxtau, "-max-tau", "--max-tau", "-no-max-tau", "--no-max-tau",
                  "Use max-tau in supg.");
   args.AddOption(&dtfactor, "-dtfactor", "--dt-factor",
                  "Tau supg scales like dt/dtfactor.");
   args.AddOption(&useFull, "-useFull", "--useFull",
                  "version of Full preconditioner");
   args.AddOption(&usefd, "-fd", "--use-fd", "-no-fd",
                  "--no-fd","Use fd-fem in the implicit solvers.");
   args.AddOption(&pa, "-pa", "--parallel-assembly", "-no-pa",
                  "--no-parallel-assembly", "Parallel assembly.");
   args.AddOption(&initial_refine, "-init-refine", "--init-refine", "-no-init-refine",
                  "--no-init-refine","Use initial refine before time stepping.");
   args.AddOption(&visit, "-visit", "--visit-datafiles", "-no-visit",
                  "--no-visit-datafiles", "Save data files for VisIt (visit.llnl.gov) visualization.");
   args.AddOption(&paraview, "-paraview", "--paraview-datafiles", "-no-paraivew",
                  "--no-paraview-datafiles", "Save data files for paraview visualization.");
   args.AddOption(&derefine, "-derefine", "--derefine-mesh", "-no-derefine",
                  "--no-derefine-mesh",
                  "Derefine the mesh in AMR.");
   args.AddOption(&error_norm, "-error-norm", "--error-norm",
                  "AMR error norm (in both refine and derefine).");
   args.AddOption(&yRange, "-yrange", "--y-refine-range", "-no-yrange",
                  "--no-y-refine-range",
                  "Refine only in the y range of [-.6, .6] in AMR.");
   args.AddOption(&use_petsc, "-usepetsc", "--usepetsc", "-no-petsc",
                  "--no-petsc",
                  "Use or not PETSc to solve the nonlinear system.");
   args.AddOption(&use_factory, "-shell", "--shell", "-no-shell",
                  "--no-shell",
                  "Use user-defined preconditioner factory (PCSHELL).");
   args.AddOption(&petscrc_file, "-petscopts", "--petscopts",
                  "PetscOptions file to use.");
   args.AddOption(&iUpdateJ, "-updatej", "--update-j",
                  "UpdateJ: 0 - no boundary condition used; 1/2 - Dirichlet used on J boundary (2: lumped mass matrix).");
   args.AddOption(&BgradJ, "-BgradJ", "--BgradJ",
                  "BgradJ: 1 - (B.grad J, phi); 2 - (-J, B.grad phi); 3 - (-B J, grad phi).");
   args.AddOption(&saveOne, "-saveOne", "--save-One",  "-no-saveOne", "--no-save-One",
                  "Save solution/mesh as one file");
   args.AddOption(&checkpt, "-checkpt", "--check-pt",  "-no-checkpt", "--no-check-pt",
                  "Save check point");
   args.AddOption(&lumpedMass, "-lumpmass", "--lump-mass",  "-no-lumpmass", "--no-lump-mass",
                  "lumped mass for updatej=0");
   args.AddOption(&iestimator, "-iestimator", "--iestimator",
                  "iestimator: 1 - psi and J; 2 - omega and psi.");
   args.AddOption(&compute_pressure, "-computep", "--compute-p", "-no-computep",
                  "--no-compute-p", "Compute pressure in the post processing");
   args.AddOption(&ref_its, "-ref-its", "--ref-its","refinement iterations.");
   args.AddOption(&deref_its, "-deref-its", "--deref-its","refinement iterations.");
   args.Parse();

   if (!args.Good())
   {
      if (myid == 0)
      {
         args.PrintUsage(cout);
      }
      MPI_Finalize();
      return 1;
   }
   if (icase==2)
   {
      resiG=resi;
   }
   else if (icase==3 || icase==4 || icase==5 || icase==6)
   {
      lambda=.5/M_PI;
      resiG=resi;
   }
   else if (icase==1)
   {
       resi=.0;
       visc=.0;
   }
   else if (icase!=1)
   {
       if (myid == 0) cout <<"Unknown icase "<<icase<<endl;
       MPI_Finalize();
       return 3;
   }
   if (myid == 0) args.PrintOptions(cout);

   if (use_petsc)
   {
      MFEMInitializePetsc(NULL,NULL,petscrc_file,NULL);
   }

   //++++Read the mesh from the given mesh file.    
   Mesh *mesh = new Mesh(mesh_file, 1, 1);
   int dim = mesh->Dimension();

   //++++Define the ODE solver used for time integration. Several implicit
   //    singly diagonal implicit Runge-Kutta (SDIRK) methods, as well as
   //    backward Euler methods are available.
   ODESolver *ode_solver=NULL;
   switch (ode_solver_type)
   {
      // Implict L-stable methods 
      case 1: ode_solver = new BackwardEulerSolver; break;
      case 3: ode_solver = new SDIRK23Solver(2); break;
      case 4: ode_solver = new SDIRK33Solver; break;
      // Implicit A-stable methods (not L-stable)
      case 12: ode_solver = new ImplicitMidpointSolver; break;
      case 13: ode_solver = new SDIRK23Solver; break;
      case 14: ode_solver = new SDIRK34Solver; break;
     default:
         if (myid == 0) cout << "Unknown ODE solver type: " << ode_solver_type << '\n';
         delete mesh;
         if (use_petsc) { MFEMFinalizePetsc(); }
         MPI_Finalize();
         return 3;
   }

   //++++Refine the mesh to increase the resolution.    
   for (int lev = 0; lev < ser_ref_levels; lev++)
   {
      mesh->UniformRefinement();
   }
   Array<int> ordering;
   mesh->GetHilbertElementOrdering(ordering);
   mesh->ReorderElements(ordering);
   mesh->EnsureNCMesh();    //note after this call all the mesh_level=0!!

   //amr_levels+=ser_ref_levels; this is not needed any more

   pmesh = new ParMesh(MPI_COMM_WORLD, *mesh);
   delete mesh;
   for (int lev = 0; lev < par_ref_levels; lev++)
   {
      pmesh->UniformRefinement();
   }
   amr_levels+=par_ref_levels;

   H1_FECollection fe_coll(order, dim);
   ParFiniteElementSpace fespace(pmesh, &fe_coll); 

   HYPRE_Int global_size = fespace.GlobalTrueVSize();
   if (myid == 0)
      cout << "Number of total scalar unknowns: " << global_size << endl;

   //this is a periodic boundary condition in x and Direchlet in y 
   Array<int> ess_bdr(fespace.GetMesh()->bdr_attributes.Max());
   ess_bdr = 0;
   ess_bdr[0] = 1;  //set attribute 1 to Direchlet boundary fixed
   if(ess_bdr.Size()!=1)
   {
    if (myid == 0) cout <<"ess_bdr size should be 1 but it is "<<ess_bdr.Size()<<endl;
    delete ode_solver;
    delete pmesh;
    if (use_petsc) { MFEMFinalizePetsc(); }
    MPI_Finalize();
    return 2;
   }

   BilinearFormIntegrator *integ = new DiffusionIntegrator;
   int fe_size, sdim = pmesh->SpaceDimension();
   //-----------------------------------Generate adaptive grid---------------------------------
   if (initial_refine)
   {
      //the first part of the code is copied from an explicit code to have a good initial adapative mesh
      //If there is a simple way to initialize the mesh, then we can drop this part.
      //But last time I tried, the solver has some issue in terms of wrong ordering and refined levels 
      //after an adaptive mesh is saved and loaded. This is a simple work around for now.
      fe_size = fespace.GetVSize();
      Array<int> fe_offset(5);
      fe_offset[0] = 0;
      fe_offset[1] = fe_size;
      fe_offset[2] = 2*fe_size;
      fe_offset[3] = 3*fe_size;
      fe_offset[4] = 4*fe_size;

      BlockVector *vxTmp = new BlockVector(fe_offset);
      ParGridFunction psiTmp, phiTmp, wTmp, jTmp;
      phiTmp.MakeRef(&fespace, vxTmp->GetBlock(0), 0);
      psiTmp.MakeRef(&fespace, vxTmp->GetBlock(1), 0);
        wTmp.MakeRef(&fespace, vxTmp->GetBlock(2), 0);
        jTmp.MakeRef(&fespace, vxTmp->GetBlock(3), 0);
      phiTmp=0.0;
        wTmp=0.0;

      ParFiniteElementSpace flux_fespace1(pmesh, &fe_coll, sdim), flux_fespace2(pmesh, &fe_coll, sdim);
      BlockZZEstimator estimatorTmp(*integ, psiTmp, *integ, phiTmp, flux_fespace1, flux_fespace2);

      ThresholdRefiner refinerTmp(estimatorTmp);
      refinerTmp.SetTotalErrorGoal(1e-7);    // total error goal (stop criterion)
      refinerTmp.SetLocalErrorGoal(1e-7);    // local error goal (stop criterion)
      refinerTmp.SetMaxElements(500000);
      refinerTmp.SetMaximumRefinementLevel(par_ref_levels+1);
      refinerTmp.SetNCLimit(nc_limit);

      AMRResistiveMHDOperator *exOperator = new AMRResistiveMHDOperator(fespace, ess_bdr, visc, resi);
      BlockVector *vxTmp_old = new BlockVector(*vxTmp);
      exOperator->assembleProblem(ess_bdr); 

      //psi is needed to get solution started
      if (icase==1)
      {
           FunctionCoefficient psiInit(InitialPsi);
           psiTmp.ProjectCoefficient(psiInit);
      }
      else if (icase==2)
      {
           FunctionCoefficient psiInit2(InitialPsi2);
           psiTmp.ProjectCoefficient(psiInit2);
      }
      else if (icase==3)
      {
           FunctionCoefficient psiInit3(InitialPsi3);
           psiTmp.ProjectCoefficient(psiInit3);
      }
      else if (icase==4)
      {
           FunctionCoefficient psiInit4(InitialPsi4);
           psiTmp.ProjectCoefficient(psiInit4);
      }
      psiTmp.SetTrueVector();

      if (icase==1)
      {
           FunctionCoefficient jInit(InitialJ);
           jTmp.ProjectCoefficient(jInit);
      }
      else if (icase==2)
      {
           FunctionCoefficient jInit2(InitialJ2);
           jTmp.ProjectCoefficient(jInit2);
      }
      else if (icase==3)
      {
           FunctionCoefficient jInit3(InitialJ3);
           jTmp.ProjectCoefficient(jInit3);
      }
      else if (icase==4)
      {
           FunctionCoefficient jInit4(InitialJ4);
           jTmp.ProjectCoefficient(jInit4);
      }
      jTmp.SetTrueVector();

      for (int ref_it = 1; ref_it<5; ref_it++)
      {
        exOperator->UpdateJ(*vxTmp, &jTmp);
        refinerTmp.Apply(*pmesh);
        if (refinerTmp.Refined()==false)
        {
            break;
        }
        else
        {
            if (myid == 0) cout<<"Initial mesh refine..."<<endl;
            AMRUpdate(*vxTmp, *vxTmp_old, fe_offset, phiTmp, psiTmp, wTmp, jTmp);
            pmesh->Rebalance();
            //---Update problem---
            AMRUpdate(*vxTmp, *vxTmp_old, fe_offset, phiTmp, psiTmp, wTmp, jTmp);
            exOperator->UpdateProblem();
            exOperator->assembleProblem(ess_bdr); 
        }
      }
      if (myid == 0) cout<<"Finish initial mesh refine..."<<endl;
      global_size = fespace.GlobalTrueVSize();
      if (myid == 0)
         cout << "Number of total scalar unknowns becomes: " << global_size << endl;
      delete vxTmp_old;
      delete vxTmp;
      delete exOperator;
   }
   //-----------------------------------End of generating adaptive grid---------------------------------

   //-----------------------------------Initial solution on adaptive grid---------------------------------
   fe_size = fespace.TrueVSize();
   Array<int> fe_offset3(4);
   fe_offset3[0] = 0;
   fe_offset3[1] = fe_size;
   fe_offset3[2] = 2*fe_size;
   fe_offset3[3] = 3*fe_size;

   BlockVector vx(fe_offset3);
   ParGridFunction phi, psi, w, j(&fespace); 
   phi.MakeTRef(&fespace, vx, fe_offset3[0]);
   psi.MakeTRef(&fespace, vx, fe_offset3[1]);
     w.MakeTRef(&fespace, vx, fe_offset3[2]);

   //+++++Set the initial conditions, and the boundary conditions
   FunctionCoefficient phiInit(InitialPhi);
   phi.ProjectCoefficient(phiInit);
   phi.SetTrueVector();
   phi.SetFromTrueVector(); 

   if (icase==1)
   {
        FunctionCoefficient psiInit(InitialPsi);
        psi.ProjectCoefficient(psiInit);
   }
   else if (icase==2)
   {
        FunctionCoefficient psiInit2(InitialPsi2);
        psi.ProjectCoefficient(psiInit2);
   }
   else if (icase==3)
   {
        FunctionCoefficient psiInit3(InitialPsi3);
        psi.ProjectCoefficient(psiInit3);
   }else if (icase==4)
   {
        FunctionCoefficient psiInit4(InitialPsi4);
        psi.ProjectCoefficient(psiInit4);
   }
   psi.SetTrueVector();
   psi.SetFromTrueVector(); 

   FunctionCoefficient wInit(InitialW);
   w.ProjectCoefficient(wInit);
   w.SetTrueVector();
   w.SetFromTrueVector();
   
   //++++Initialize the MHD operator, the GLVis visualization    
   ResistiveMHDOperator oper(fespace, ess_bdr, visc, resi, use_petsc, use_factory);
   //add the source term
   if (icase==2){
       oper.SetRHSEfield(E0rhs);
   }
   else if (icase==3 || icase==4){
       oper.SetRHSEfield(E0rhs3);
   }

   //set initial J
   FunctionCoefficient jInit1(InitialJ), jInit2(InitialJ2), jInit3(InitialJ3), jInit4(InitialJ4), *jptr;
   if (icase==1)
       jptr=&jInit1;
   else if (icase==2)
       jptr=&jInit2;
   else if (icase==3)
       jptr=&jInit3;
   else if (icase==4)
       jptr=&jInit4;
   j.ProjectCoefficient(*jptr);
   j.SetTrueVector();
   oper.SetInitialJ(*jptr);

   //-----------------------------------AMR for the real computation---------------------------------
   ErrorEstimator *estimator_used;
   BlockZZEstimator *estimator=NULL;
   BlockL2ZZEstimator *L2estimator=NULL;
   ParFiniteElementSpace flux_fespace1(pmesh, &fe_coll, sdim), flux_fespace2(pmesh, &fe_coll, sdim);
   RT_FECollection smooth_flux_fec(order-1, dim);
   ParFiniteElementSpace smooth_flux_fes1(pmesh, &smooth_flux_fec), smooth_flux_fes2(pmesh, &smooth_flux_fec);

   bool regularZZ=true;
   if (regularZZ)
   {
     if (iestimator==1){
        estimator=new BlockZZEstimator(*integ, psi, *integ, j, flux_fespace1, flux_fespace2);
     }
     else{
        estimator=new BlockZZEstimator(*integ, w, *integ, psi, flux_fespace1, flux_fespace2);
     }
     estimator->SetErrorRatio(err_ratio); 
     estimator_used=estimator;
   }
   else{
     L2estimator=new BlockL2ZZEstimator(*integ, psi, *integ, j, flux_fespace1, flux_fespace2, smooth_flux_fes1,smooth_flux_fes2);
     L2estimator->SetErrorRatio(err_ratio); 
     estimator_used=L2estimator;
   }

   int levels3=par_ref_levels+3, levels4=par_ref_levels+4;
   ThresholdRefiner refiner(*estimator_used);
   refiner.SetTotalErrorFraction(err_fraction);   // here default is 0.5, mean refine error >0.5*total_error
   refiner.SetTotalErrorGoal(0.0);  // total error goal (stop criterion)
   refiner.SetLocalErrorGoal(ltol_amr);  // local error goal (stop criterion)
   refiner.SetTotalErrorNormP(error_norm);
   refiner.SetMaxElements(10000000);
   if (levels3<amr_levels)
      refiner.SetMaximumRefinementLevel(levels3);
   else
      refiner.SetMaximumRefinementLevel(amr_levels);
   refiner.SetNCLimit(nc_limit);
   if (yRange)
       refiner.SetYRange(-.6, .6);

   ThresholdDerefiner derefiner(*estimator_used);
   derefiner.SetThreshold(derefine_ratio*ltol_amr);
   derefiner.SetNCLimit(nc_limit);
   derefiner.SetTotalErrorNormP(error_norm);
   if (derefine_fraction>=err_fraction && derefine)
   {   
       if (myid==0) cout << "ERROR: derefine_fraction is set to be large than err_fraction!!"<<endl;
       if (use_petsc) { MFEMFinalizePetsc(); }
       delete ode_solver;
       delete pmesh;
       delete integ;
       delete estimator_used;
       MPI_Finalize();
       return 3;
   }
   else
   { derefiner.SetTotalErrorFraction(derefine_fraction); }

   bool derefineMesh = false;
   bool refineMesh = false;
   //-----------------------------------AMR---------------------------------

   socketstream vis_phi, vis_j, vis_psi, vis_w;
   if (visualization)
   {
      char vishost[] = "localhost";
      int  visport   = 19916;
      vis_phi.open(vishost, visport);
      if (!vis_phi)
      {
          if (myid==0)
          {
            cout << "Unable to connect to GLVis server at "
                 << vishost << ':' << visport << endl;
            cout << "GLVis visualization disabled.\n";
          }
         visualization = false;
      }
      else
      {
         vis_phi << "parallel " << num_procs << " " << myid << "\n";
         vis_phi.precision(8);
         vis_phi << "solution\n" << *pmesh << phi;
         vis_phi << "window_size 800 800\n"<< "window_title '" << "phi'" << "keys cm\n";
         vis_phi << flush;

         vis_j.open(vishost, visport);
         vis_j << "parallel " << num_procs << " " << myid << "\n";
         vis_j.precision(8);
         vis_j << "solution\n" << *pmesh << j;
         vis_j << "window_size 800 800\n"<< "window_title '" << "current'" << "keys cm\n";
         vis_j << flush;
         MPI_Barrier(MPI_COMM_WORLD);//without barrier, glvis may not open

         vis_w.open(vishost, visport);
         vis_w << "parallel " << num_procs << " " << myid << "\n";
         vis_w.precision(8);
         vis_w << "solution\n" << *pmesh << w;
         vis_w << "window_size 800 800\n"<< "window_title '" << "omega'" << "keys cm\n";
         vis_w << flush;
         MPI_Barrier(MPI_COMM_WORLD);
      }
   }

   double t = 0.0;
   oper.SetTime(t);
   ode_solver->Init(oper);

   // Create data collection for solution output: either VisItDataCollection for
   // ascii data files, or SidreDataCollection for binary data files.
   DataCollection *dc = NULL;
   if (visit)
   {
      if (icase==1)
      {
        dc = new VisItDataCollection("case1", pmesh);
        dc->RegisterField("psi", &psi);
      }
      else if (icase==2)
      {
        dc = new VisItDataCollection("case2", pmesh);
        dc->RegisterField("psi", &psi);
        dc->RegisterField("phi", &phi);
        dc->RegisterField("omega", &w);
      }
      else
      {
        dc = new VisItDataCollection("case3", pmesh);
        dc->RegisterField("psi", &psi);
        dc->RegisterField("phi", &phi);
        dc->RegisterField("omega", &w);
      }
      dc->RegisterField("j", &j);

      bool par_format = false;
      dc->SetFormat(!par_format ?
                      DataCollection::SERIAL_FORMAT :
                      DataCollection::PARALLEL_FORMAT);
      dc->SetPrecision(5);
      dc->SetCycle(0);
      dc->SetTime(t);
      dc->Save();
   }

   //save domain decompositino explicitly
   L2_FECollection pw_const_fec(0, dim);
   ParFiniteElementSpace pw_const_fes(pmesh, &pw_const_fec);
   ParGridFunction mpi_rank_gf(&pw_const_fes);
   mpi_rank_gf = myid_rand;
   
   //++++recover pressure and vector fields++++
   ParFiniteElementSpace *vfes;
   ParGridFunction *vel, *mag, *gradP, *BgradB, *gradBP, *gfv, *pre=NULL;
   ParMixedBilinearForm *grad, *div;
   ParBilinearForm *a;
   ParNonlinearForm *convect;
   ParLinearForm *zLF, *zLFscalar; 
   ParBilinearForm *Mfull, *Mrot;
   Vector zv, zv2, zscalar, zscalar2;
   HypreParMatrix *MfullMat, *KMat;
   const IntegrationRule &ir = IntRules.Get(fespace.GetFE(0)->GetGeomType(), 3*order);
   CGSolver M_solver(MPI_COMM_WORLD), Mscal_solver;
   HypreSmoother *M_prec;
   HypreBoomerAMG *K_amg;
   CGSolver *K_pcg;
   mfem::navier::OrthoSolver *SpInvOrthoPC;
   Vector vtrue, rhs, vJxB;
   VectorDomainLFIntegrator *domainJxB;
   bool vfes_match=false;

   if(compute_pressure)
   {
      vfes = new ParFiniteElementSpace(pmesh, fespace.FEColl(), 2);
      vel = new ParGridFunction(vfes);
      mag = new ParGridFunction(vfes);
      gradP = new ParGridFunction(vfes);
      BgradB = new ParGridFunction(vfes);
      gradBP = new ParGridFunction(vfes);
      gfv = new ParGridFunction(vfes);
      pre = new ParGridFunction(&fespace);
      grad = new ParMixedBilinearForm(&fespace, vfes);
      div = new ParMixedBilinearForm(vfes, &fespace);
      convect = new ParNonlinearForm(vfes);
      zLF  = new ParLinearForm(vfes);
      zLFscalar = new ParLinearForm(&fespace);
      Mfull = new ParBilinearForm(vfes);
      Mrot = new ParBilinearForm(vfes);
      Mscal_solver=oper.GetM_solver2();

      int vfes_truevsize = vfes->GetTrueVSize();
      vtrue.SetSize(vfes_truevsize);
      rhs.SetSize(vfes_truevsize);
      vJxB.SetSize(vfes_truevsize);

      DenseMatrix A(2);
      A(0,0) = 0.0; A(0,1) =-1.0;
      A(1,0) = 1.0; A(1,1) = 0.0;
      MatrixConstantCoefficient coeff_curl(A);

      //mass matrix for vector fields
      Mfull->AddDomainIntegrator(new VectorMassIntegrator);
      Mfull->Assemble();
      Mfull->Finalize();
      MfullMat = Mfull->ParallelAssemble();

      M_solver.iterative_mode = false;
      M_solver.SetRelTol(1e-7);
      M_solver.SetAbsTol(0.0);
      M_solver.SetMaxIter(2000);
      M_solver.SetPrintLevel(0);
      M_prec = new HypreSmoother;  
      M_prec->SetType(HypreSmoother::Jacobi);
      M_solver.SetPreconditioner(*M_prec);
      M_solver.SetOperator(*MfullMat);

      //gradient operator from H1 to Vector H1
      grad->AddDomainIntegrator(new GradientIntegrator);
      grad->Assemble(); 

      //nonlinear convection term u.grad u
      convect->AddDomainIntegrator(new VectorConvectionNLFIntegrator);
      convect->Setup();

      //divergence operator from Vector H1 to H1
      div->AddDomainIntegrator(new VectorDivergenceIntegrator);
      div->Assemble();

      //rotation matrix
      Mrot->AddDomainIntegrator(new VectorMassIntegrator(coeff_curl));
      Mrot->Assemble();
      Mrot->Finalize();

       zv.SetSize(vfes->TrueVSize());
      zv2.SetSize(vfes->TrueVSize());
      zscalar.SetSize(fespace.TrueVSize());
      zscalar2.SetSize(fespace.TrueVSize());

      //compute velocity 
      grad->Mult(phi, *zLF);
      zLF->ParallelAssemble(zv);
      M_solver.Mult(zv, zv2);
      vel->SetFromTrueDofs(zv2);

      //finalize with a rotation
      Mrot->Mult(*vel, *zLF);
      zLF->ParallelAssemble(zv);
      M_solver.Mult(zv, zv2);
      vel->SetFromTrueDofs(zv2);

      //compute B field
      grad->Mult(psi, *zLF);
      zLF->ParallelAssemble(zv);
      M_solver.Mult(zv, zv2);
      mag->SetFromTrueDofs(zv2);

      //finalize with a rotation
      Mrot->Mult(*mag, *zLF);
      zLF->ParallelAssemble(zv);
      M_solver.Mult(zv, zv2);
      mag->SetFromTrueDofs(zv2);

      //compute -Δp=div(u.grad u - JxB)
      vel->GetTrueDofs(vtrue);
      convect->Mult(vtrue, rhs);  //nonlinear form only works with true dofs?

      JxBCoefficient JxBCoeff(&j, mag);
      domainJxB = new VectorDomainLFIntegrator(JxBCoeff);
      domainJxB->SetIntRule(&ir);
      ParLinearForm zJxB(vfes);
      zJxB.AddDomainIntegrator(domainJxB);
      zJxB.Assemble();
      zJxB.ParallelAssemble(vJxB);
      rhs.Add(-1.0, vJxB);
      
      //compute M^{-1}(u.grad u - JxB)
      M_solver.Mult(rhs, zv2);
      gfv->SetFromTrueDofs(zv2);
      div->Mult(*gfv, *zLFscalar);  //it is a mystery why ParGridFunction fails here

      a = new ParBilinearForm(&fespace);
      a->AddDomainIntegrator(new DiffusionIntegrator);
      a->Assemble();
      a->Finalize();
      KMat=a->ParallelAssemble();

      K_amg = new HypreBoomerAMG(*KMat);
      K_amg->SetPrintLevel(0);
      K_pcg = new CGSolver(MPI_COMM_WORLD);
      SpInvOrthoPC = new mfem::navier::OrthoSolver();
      SpInvOrthoPC->SetOperator(*K_amg);
      K_pcg->SetOperator(*KMat);
      K_pcg->iterative_mode = false;
      K_pcg->SetRelTol(1e-7);
      K_pcg->SetMaxIter(200);
      K_pcg->SetPrintLevel(0);
      K_pcg->SetPreconditioner(*SpInvOrthoPC);

      ParLinearForm b(&fespace);
      b.AddBoundaryIntegrator(new BoundaryNormalLFIntegrator(JxBCoeff), ess_bdr);
      b.Assemble();
      b.ParallelAssemble(zscalar);

      zLFscalar->ParallelAssemble(zscalar2);
      zscalar.Add(1.0, zscalar2);
      K_pcg->Mult(zscalar, zscalar2);
      pre->SetFromTrueDofs(zscalar2);

      //compute grad P
      zv=0.0;
      grad->TrueAddMult(zscalar2, zv);
      M_solver.Mult(zv, zv2);
      gradP->SetFromTrueDofs(zv2);

      //compute B.gradB
      mag->GetTrueDofs(vtrue);
      convect->Mult(vtrue, zv);  
      M_solver.Mult(zv, zv2);
      BgradB->SetFromTrueDofs(zv2);

      //compute grad magnetic pressure
      B2Coefficient B2Coeff(mag);
      ParLinearForm B2int(&fespace);
      B2int.AddDomainIntegrator(new DomainLFIntegrator(B2Coeff, 2, 0));
      B2int.Assemble();
      B2int.ParallelAssemble(zscalar);
      Mscal_solver.Mult(zscalar, zscalar2);
      zv=0.0;
      grad->TrueAddMult(zscalar2, zv);
      M_solver.Mult(zv, zv2);
      gradBP->SetFromTrueDofs(zv2);

      vfes_match=true;
   }

   ParaViewDataCollection *pd = NULL;
   if (paraview)
   {
      pd = new ParaViewDataCollection("imAMRMHD", pmesh);
      pd->SetPrefixPath("ParaView");
      pd->RegisterField("psi", &psi);
      pd->RegisterField("phi", &phi);
      pd->RegisterField("omega", &w);
      pd->RegisterField("current", &j);
      pd->RegisterField("MPI rank", &mpi_rank_gf);
      if (compute_pressure){
          pd->RegisterField("V", vel);
          pd->RegisterField("B", mag);
          pd->RegisterField("pre", pre);
          pd->RegisterField("grad pre", gradP);
          pd->RegisterField("grad mag pre", gradBP);
          pd->RegisterField("B.gradB", BgradB);
      }
      pd->SetLevelsOfDetail(order);
      pd->SetDataFormat(VTKFormat::BINARY);
      pd->SetHighOrderOutput(true);
      pd->SetCycle(0);
      pd->SetTime(0.0);
      pd->Save();
   }

   MPI_Barrier(MPI_COMM_WORLD); 
   double start = MPI_Wtime();

   if (myid == 0) cout<<"Start time stepping..."<<endl;

   //++++Perform time-integration (looping over the time iterations, ti, with a time-step dt).
   bool last_step = false;
   for (int ti = 1; !last_step; ti++)
   {
      //this solver does not support reduce dt automatically when snes fails
      if (t_change>0. && t>=t_change)
      {
        dt=dt/2.;
        if (myid==0) cout << "change time step to "<<dt<<endl;
        t_change=0.;
      }

      double dt_real = min(dt, t_final - t);

      if (t>t_refs)
      {
          ref_steps=t_refs_steps;
          ref_its=1;
          deref_its=1;
      }

      if (t>4. && levels3<amr_levels)
      {
          refiner.SetMaximumRefinementLevel(amr_levels);
      }

      if ((ti % ref_steps) == 0)
      {
          refineMesh=true;
          refiner.Reset();
          derefineMesh=true;
          derefiner.Reset();
      }
      else
      {
          refineMesh=false;
          derefineMesh=false;
      }

      //---the main solve step---
      ode_solver->Step(vx, t, dt_real);

      last_step = (t >= t_final - 1e-8*dt);
      if (last_step)
      {
          refineMesh=false;
          derefineMesh=false;
      }

      //update J and psi as it is needed in the refine or derefine step
      if (refineMesh || derefineMesh)
      {
          phi.SetFromTrueDofs(vx.GetBlock(0));
          psi.SetFromTrueDofs(vx.GetBlock(1));
          w.SetFromTrueDofs(vx.GetBlock(2));
      }

      if (myid == 0)
      {
          global_size = fespace.GlobalTrueVSize();
          cout << "Number of total scalar unknowns: " << global_size << endl;
          cout << "step " << ti << ", t = " << t <<endl;
      }

      //----------------------------AMR---------------------------------
      
      //++++++Refine step++++++
      if (refineMesh)  
      {
         if (myid == 0) cout<<"Refine mesh iterations..."<<endl;

         int its;
         //here can we skip replacing??
         for (its=0; its<ref_its; its++)
         {
           oper.UpdateJ(vx, &j);
           if (refiner.Apply(*pmesh)==false)
           {
               if (myid == 0) cout<<"No refined element found. Skip..."<<endl;
               break;
           }

           AMRUpdateTrue(vx, fe_offset3, phi, psi, w, j, pre);
           oper.UpdateGridFunction();
           if (compute_pressure) {
               vfes->Update();
               //update vector grid function
               vel->Update();
               mag->Update();
               gradP->Update();
               BgradB->Update();
               gradBP->Update();
               gfv->Update();
               vfes->UpdatesFinished();
           }
           if (paraview) 
           {
               pw_const_fes.Update();
               mpi_rank_gf.Update();
           }

           pmesh->Rebalance();

           if (paraview) 
           {
               pw_const_fes.Update();
               mpi_rank_gf.Update();
           }

           //---Update solutions after rebalancing---
           AMRUpdateTrue(vx, fe_offset3, phi, psi, w, j, pre);
           oper.UpdateGridFunction();
           if (compute_pressure) {
               vfes->Update();
               //update vector grid function
               vel->Update();
               mag->Update();
               gradP->Update();
               BgradB->Update();
               gradBP->Update();
               gfv->Update();
               vfes->UpdatesFinished();
           }
           oper.UpdateProblem(ess_bdr); 
           oper.SetInitialJ(*jptr);      //need to reset the current bounary

           if (myid == 0)
           {
             global_size = fespace.GlobalTrueVSize();
             cout << "Number of total scalar unknowns: " << global_size <<"; amr it= "<<its<<endl; 
           }
         }

         //upate ode_solver for the next time step
         if (its>0 || refiner.Refined())
         {
            if (myid == 0) cout<<"Refined mesh; initialize ode_solver"<<endl;
            ode_solver->Init(oper);
            if (compute_pressure) {
               if (myid == 0) cout << "Mesh has changed and rebuilding vfes is needed"<<endl;
               vfes_match = false;
            }
         }
      }

      //++++++Derefine step++++++
      if (derefineMesh && derefine)
      {
         if (myid == 0) cout << "Derefined mesh..." << endl;

         int its;
         for (its=0; its<deref_its; its++)
         {
             oper.UpdateJ(vx, &j);
             if (!derefiner.Apply(*pmesh))
             {
                 if (myid == 0) cout << "No derefine elements found, skip..." << endl;
                 break;
             }

             //---Update solutions first---
             AMRUpdateTrue(vx, fe_offset3, phi, psi, w, j, pre);
             oper.UpdateGridFunction();
             if (compute_pressure) {
                vfes->Update();
                //update vector grid function
                vel->Update();
                mag->Update();
                gradP->Update();
                BgradB->Update();
                gradBP->Update();
                gfv->Update();
                vfes->UpdatesFinished();
             }

             if (paraview) 
             {
                 pw_const_fes.Update();
                 mpi_rank_gf.Update();
             }

             pmesh->Rebalance();

             if (paraview) 
             {
                 pw_const_fes.Update();
                 mpi_rank_gf.Update();
             }

             //---Update solutions after rebalancing---
             AMRUpdateTrue(vx, fe_offset3, phi, psi, w, j, pre);
             oper.UpdateGridFunction();
             if (compute_pressure) {
                vfes->Update();
                //update vector grid function
                vel->Update();
                mag->Update();
                gradP->Update();
                BgradB->Update();
                gradBP->Update();
                gfv->Update();
                vfes->UpdatesFinished();
             }

             //---assemble problem and update boundary condition---
             oper.UpdateProblem(ess_bdr); 
             oper.SetInitialJ(*jptr);    //somehow I need to reset the current bounary

             if (myid == 0)
             {
               global_size = fespace.GlobalTrueVSize();
               cout << "Number of total scalar unknowns: " << global_size <<"; amr it= "<<its<<endl;
             }
         }

         if (its>0 || derefiner.Derefined())
         {
            if (myid == 0) cout<<"Derefined mesh; initialize ode_solver"<<endl;
            ode_solver->Init(oper);
            if (compute_pressure) {
               if (myid == 0)  cout << "Mesh has changed and rebuilding vfes is needed"<<endl;
               vfes_match = false;
            }
         }
      }
      //----------------------------AMR---------------------------------

      if ( (last_step || (ti % vis_steps) == 0) )
      {
        if (visualization || visit || paraview)
        {
           phi.SetFromTrueDofs(vx.GetBlock(0));
           psi.SetFromTrueDofs(vx.GetBlock(1));
           w.SetFromTrueDofs(vx.GetBlock(2));
           oper.UpdateJ(vx, &j);

           if(compute_pressure && paraview)
           {
              if (!vfes_match){
                delete grad;     
                delete div ;     
                delete convect;  
                delete zLF  ;    
                delete zLFscalar; 
                delete Mfull;    
                delete Mrot ;    
                delete M_prec;
                delete MfullMat;
                delete a;
                delete KMat;
                delete K_amg;
                delete K_pcg;
                delete SpInvOrthoPC;

                grad = new ParMixedBilinearForm(&fespace, vfes);
                div = new ParMixedBilinearForm(vfes, &fespace);
                convect = new ParNonlinearForm(vfes);
                zLF  = new ParLinearForm(vfes);
                zLFscalar = new ParLinearForm(&fespace);
                Mfull = new ParBilinearForm(vfes);
                Mrot = new ParBilinearForm(vfes);
                Mscal_solver=oper.GetM_solver2();

                int vfes_truevsize = vfes->GetTrueVSize();
                vtrue.SetSize(vfes_truevsize);
                rhs.SetSize(vfes_truevsize);
                vJxB.SetSize(vfes_truevsize);

                DenseMatrix A(2);
                A(0,0) = 0.0; A(0,1) =-1.0;
                A(1,0) = 1.0; A(1,1) = 0.0;
                MatrixConstantCoefficient coeff_curl(A);

                //mass matrix for vector fields
                Mfull->AddDomainIntegrator(new VectorMassIntegrator);
                Mfull->Assemble();
                Mfull->Finalize();
                MfullMat = Mfull->ParallelAssemble();

                M_solver.iterative_mode = false;
                M_solver.SetRelTol(1e-7);
                M_solver.SetAbsTol(0.0);
                M_solver.SetMaxIter(2000);
                M_solver.SetPrintLevel(0);
                M_prec = new HypreSmoother;  
                M_prec->SetType(HypreSmoother::Jacobi);
                M_solver.SetPreconditioner(*M_prec);
                M_solver.SetOperator(*MfullMat);

                //gradient operator from H1 to Vector H1
                grad->AddDomainIntegrator(new GradientIntegrator);
                grad->Assemble(); 

                //nonlinear convection term u.grad u
                convect->AddDomainIntegrator(new VectorConvectionNLFIntegrator);
                convect->Setup();

                //divergence operator from Vector H1 to H1
                div->AddDomainIntegrator(new VectorDivergenceIntegrator);
                div->Assemble();

                //rotation matrix
                Mrot->AddDomainIntegrator(new VectorMassIntegrator(coeff_curl));
                Mrot->Assemble();
                Mrot->Finalize();

                 zv.SetSize(vfes->TrueVSize());
                zv2.SetSize(vfes->TrueVSize());
                zscalar.SetSize(fespace.TrueVSize());
                zscalar2.SetSize(fespace.TrueVSize());

                a = new ParBilinearForm(&fespace);
                a->AddDomainIntegrator(new DiffusionIntegrator);
                a->Assemble();
                a->Finalize();
                KMat=a->ParallelAssemble();

                K_amg = new HypreBoomerAMG(*KMat);
                K_amg->SetPrintLevel(0);
                K_pcg = new CGSolver(MPI_COMM_WORLD);
                SpInvOrthoPC = new mfem::navier::OrthoSolver();
                SpInvOrthoPC->SetOperator(*K_amg);
                K_pcg->SetOperator(*KMat);
                K_pcg->iterative_mode = false;
                K_pcg->SetRelTol(1e-7);
                K_pcg->SetMaxIter(200);
                K_pcg->SetPrintLevel(0);
                K_pcg->SetPreconditioner(*SpInvOrthoPC);
               
                vfes_match=true;
              }

              //compute velocity 
              grad->Mult(phi, *zLF);
              zLF->ParallelAssemble(zv);
              M_solver.Mult(zv, zv2);
              vel->SetFromTrueDofs(zv2);

              //finalize with a rotation
              Mrot->Mult(*vel, *zLF);
              zLF->ParallelAssemble(zv);
              M_solver.Mult(zv, zv2);
              vel->SetFromTrueDofs(zv2);

              //compute B field
              grad->Mult(psi, *zLF);
              zLF->ParallelAssemble(zv);
              M_solver.Mult(zv, zv2);
              mag->SetFromTrueDofs(zv2);

              //finalize with a rotation
              Mrot->Mult(*mag, *zLF);
              zLF->ParallelAssemble(zv);
              M_solver.Mult(zv, zv2);
              mag->SetFromTrueDofs(zv2);

              //compute -Δp=div(u.grad u - JxB)
              vel->GetTrueDofs(vtrue);
              convect->Mult(vtrue, rhs);  //nonlinear form only works with true dofs?

              JxBCoefficient JxBCoeff(&j, mag);
              domainJxB = new VectorDomainLFIntegrator(JxBCoeff);
              domainJxB->SetIntRule(&ir);
              ParLinearForm zJxB(vfes);
              zJxB.AddDomainIntegrator(domainJxB);
              zJxB.Assemble();
              zJxB.ParallelAssemble(vJxB);
              rhs.Add(-1.0, vJxB);
              
              //compute M^{-1}(u.grad u - JxB)
              M_solver.Mult(rhs, zv2);
              gfv->SetFromTrueDofs(zv2);
              div->Mult(*gfv, *zLFscalar);  //it is a mystery why ParGridFunction fails here

              ParLinearForm b(&fespace);
              b.AddBoundaryIntegrator(new BoundaryNormalLFIntegrator(JxBCoeff), ess_bdr);
              b.Assemble();
              b.ParallelAssemble(zscalar);

              zLFscalar->ParallelAssemble(zscalar2);
              zscalar.Add(1.0, zscalar2);
              K_pcg->Mult(zscalar, zscalar2);
              pre->SetFromTrueDofs(zscalar2);

              //compute grad P
              zv=0.0;
              grad->TrueAddMult(zscalar2, zv);
              M_solver.Mult(zv, zv2);
              gradP->SetFromTrueDofs(zv2);

              //compute B.gradB
              mag->GetTrueDofs(vtrue);
              convect->Mult(vtrue, zv);  
              M_solver.Mult(zv, zv2);
              BgradB->SetFromTrueDofs(zv2);

              //compute grad magnetic pressure
              B2Coefficient B2Coeff(mag);
              ParLinearForm B2int(&fespace);
              B2int.AddDomainIntegrator(new DomainLFIntegrator(B2Coeff, 2, 0));
              B2int.Assemble();
              B2int.ParallelAssemble(zscalar);
              Mscal_solver.Mult(zscalar, zscalar2);
              zv=0.0;
              grad->TrueAddMult(zscalar2, zv);
              M_solver.Mult(zv, zv2);
              gradBP->SetFromTrueDofs(zv2);
           }
        }

        if (visualization)
        {
           vis_phi << "parallel " << num_procs << " " << myid << "\n";
           vis_phi << "solution\n" << *pmesh << phi;
           if (icase==1) 
               vis_phi << "valuerange -.001 .001\n" << flush;
           else
               vis_phi << flush;

           vis_j << "parallel " << num_procs << " " << myid << "\n";
           vis_j << "solution\n" << *pmesh << j << flush;
           vis_w << "parallel " << num_procs << " " << myid << "\n";
           vis_w << "solution\n" << *pmesh << w << flush;
        }

        if (visit)
        {
           dc->SetCycle(ti);
           dc->SetTime(t);
           dc->Save();
        }

        if (paraview)
        {
           mpi_rank_gf = myid_rand;
           pd->SetCycle(ti);
           pd->SetTime(t);
           pd->Save();
        }
      }

      if (last_step)
          break;
      else
          continue;
   }

   MPI_Barrier(MPI_COMM_WORLD); 
   double end = MPI_Wtime();

   //++++++Save the solutions.
   if (checkpt)
   {
      phi.SetFromTrueDofs(vx.GetBlock(0));
      psi.SetFromTrueDofs(vx.GetBlock(1));
      w.SetFromTrueDofs(vx.GetBlock(2));

      ofstream ofs_mesh(MakeParFilename("mesh.", myid));
      ofstream ofs_phi(MakeParFilename("checkpt-phi.", myid));
      ofstream ofs_psi(MakeParFilename("checkpt-psi.", myid));
      ofstream   ofs_w(MakeParFilename("checkpt-w.", myid));

      ofs_mesh.precision(8);
      ofs_phi.precision(16);
      ofs_psi.precision(16);
        ofs_w.precision(16);

      pmesh->ParPrint(ofs_mesh);
      phi.Save(ofs_phi);
      psi.Save(ofs_psi);
        w.Save(ofs_w);

      if (!paraview && !visit)
      {
        ostringstream j_name;
        j_name << "sol_j." << setfill('0') << setw(6) << myid;

        oper.UpdateJ(vx, &j);
        ofstream osol5(j_name.str().c_str());
        osol5.precision(8);
        j.Save(osol5);

        //output v1 and v2 for a comparision
        ParGridFunction v1(&fespace), v2(&fespace);
        oper.computeV(&phi, &v1, &v2);

        ostringstream v1_name, v2_name;
        v1_name << "sol_v1." << setfill('0') << setw(6) << myid;
        v2_name << "sol_v2." << setfill('0') << setw(6) << myid;
        ofstream osol6(v1_name.str().c_str());
        osol6.precision(8);
        v1.Save(osol6);

        ofstream osol7(v2_name.str().c_str());
        osol7.precision(8);
        v2.Save(osol7);

        ParGridFunction b1(&fespace), b2(&fespace);
        oper.computeV(&psi, &b1, &b2);
        ostringstream b1_name, b2_name;
        b1_name << "sol_b1." << setfill('0') << setw(6) << myid;
        b2_name << "sol_b2." << setfill('0') << setw(6) << myid;
        ofstream osol8(b1_name.str().c_str());
        osol8.precision(8);
        b1.Save(osol8);

        ofstream osol9(b2_name.str().c_str());
        osol9.precision(8);
        b2.Save(osol9);
      }
   }

   if (saveOne)
   {
      phi.SetFromTrueDofs(vx.GetBlock(0));
      psi.SetFromTrueDofs(vx.GetBlock(1));
      w.SetFromTrueDofs(vx.GetBlock(2));
      oper.UpdateJ(vx, &j);

      ostringstream mesh_name, j_name, psi_name, phi_name, w_name;
      mesh_name << "amr.mesh";
      j_name << "j.sol";
      w_name << "w.sol";
      phi_name << "phi.sol";
      psi_name << "psi.sol";

      ofstream mesh_ofs(mesh_name.str().c_str());
      ofstream osolj(j_name.str().c_str());
      ofstream osolw(w_name.str().c_str());
      ofstream osolphi(phi_name.str().c_str());
      ofstream osolpsi(psi_name.str().c_str());

      mesh_ofs.precision(8);
      osolj.precision(8);
      osolw.precision(8);
      osolphi.precision(8);
      osolpsi.precision(8);

      pmesh->PrintAsOne(mesh_ofs);

      j.SaveAsOne(osolj);
      w.SaveAsOne(osolw);
      phi.SaveAsOne(osolphi);
      psi.SaveAsOne(osolpsi);
   }

   if (myid == 0) 
   { 
       cout <<"######Runtime = "<<end-start<<" ######"<<endl;
   }

   if(compute_pressure)
   {
      delete M_prec;
      delete K_amg;
      delete SpInvOrthoPC;
      delete MfullMat;
      delete KMat;
      delete a;
      delete K_pcg;
      delete vfes;
      delete vel;
      delete mag;
      delete gradP;
      delete gradBP;
      delete BgradB;
      delete gfv;       
      delete zLFscalar; 
      delete pre;      
      delete grad;     
      delete div ;     
      delete convect;  
      delete zLF  ;    
      delete Mfull;    
      delete Mrot ;     
   }

   //+++++Free the used memory.
   delete ode_solver;
   delete pmesh;
   delete integ;
   delete dc;
   delete pd;
   delete estimator_used;

   oper.DestroyHypre();

   if (use_petsc) { MFEMFinalizePetsc(); }

   MPI_Finalize();

   return 0;
}


