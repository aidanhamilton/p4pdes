static char help[] =
"Time-dependent pure-advection equation, in flux-conservative form, in 2D\n"
"using TS.  Option prefix -adv_.  Domain is (-1,1) x (-1,1).  Equation is\n"
"  u_t + div(a(x,y) u) = g(x,y,u).\n"
"Boundary conditions are periodic in x and y.  Cells are grid-point centered.\n"
"Uses flux-limited (non-oscillatory) method-of-lines discretization\n"
"(Hundsdorfer & Verwer 2003). Limiters are van Leer (1974) [default],\n"
"Koren (1993), centered, or none (= first-order upwind).\n\n";

#include <petsc.h>

// try:
//   ./advect -da_refine 5 -ts_monitor_solution draw -ts_monitor -ts_rk_type 5dp -adv_limiter koren

// one lap of circular motion, computed in parallel, reproducing LeVeque Figure 20.5:
//   mpiexec -n 4 ./advect -da_grid_x 80 -da_grid_y 80 -ts_monitor_solution draw -ts_monitor -ts_rk_type 2a -adv_problem rotation -ts_final_time 3.1415926

// above is really fast with -ts_rk_type 5f|5dp|3bs

// implicit and evidence that smoother limiter is better: succeeds with XX=none,centered,vanleer; big iteration counts and then fails for koren; note "-snes_mf_operator" works with vanleer but hangs with koren
// mpiexec -n 4 ./advect -ts_monitor_solution draw -ts_monitor -adv_problem rotation -ts_final_time 0.2 -ts_type cn -da_refine 6 -snes_monitor -ts_dt 0.02 -snes_fd_color -adv_limiter XX

// with -adv_limiter none, -snes_type test suggests Jacobian is correct

// see petsc pull request https://bitbucket.org/petsc/petsc/pull-requests/662/barry-fix-even-huger-flaw-in-ts/diff regarding -snes_mf_operator

//STARTLIMITER
/* the centered-space method is a trivial (and poor) limiter */
static double centered(double th) {
    return 0.5;
}

/* the van Leer (1974) limiter is formula (1.11) in section III.1 of
Hundsdorfer & Verwer */
static double vanleer(double th) {
    return 0.5 * (th + PetscAbsReal(th)) / (1.0 + PetscAbsReal(th));
}

/* the Koren (1993) limiter is formula (1.7) in same source */
static double koren(double th) {
    const double z = (1.0/3.0) + (1.0/6.0) * th;
    return PetscMax(0.0, PetscMin(1.0, PetscMin(z, th)));
}

typedef enum {NONE, CENTERED, VANLEER, KOREN} LimiterType;
static const char *LimiterTypes[] = {"none","centered","vanleer","koren",
                                     "LimiterType", "", NULL};
static void* limiterptr[] = {NULL, &centered, &vanleer, &koren};
//ENDLIMITER

//STARTCTX
typedef enum {ROTATION, STRAIGHT} ProblemType;
static const char *ProblemTypes[] = {"rotation","straight",
                                     "ProblemType", "", NULL};

typedef struct {
    ProblemType problem;
    double      windx, windy; // x,y components of wind (if problem = STRAIGHT)
    double      (*limiter)(double);
} AdvectCtx;
//ENDCTX

PetscErrorCode FormInitial(DMDALocalInfo *info, Vec u, AdvectCtx* user) {
    PetscErrorCode ierr;
    int          i, j;
    double       hx, hy, x, y, r, **au;

    ierr = VecSet(u,0.0); CHKERRQ(ierr);  // clear it first
    ierr = DMDAVecGetArray(info->da, u, &au); CHKERRQ(ierr);
    hx = 2.0 / info->mx;  hy = 2.0 / info->my;
    for (j=info->ys; j<info->ys+info->ym; j++) {
        y = -1.0 + (j+0.5) * hy;
        for (i=info->xs; i<info->xs+info->xm; i++) {
            x = -1.0 + (i+0.5) * hx;
            switch (user->problem) {
                case STRAIGHT:
                    // goal: reproduce Figure 6.2, page 303, in
                    // Hundsdorfer & Verwer (2003). "Numerical Solution of
                    // Time-Dependent Advection-Diffusion-Reaction Equations",
                    // Springer; but scaled by factor of 2
                    r = PetscSqrtReal((x+0.6)*(x+0.6) + (y+0.6)*(y+0.6));
                    if (r < 0.2) {
                        au[j][i] = 1.0;
                    }
                    break;
                case ROTATION:
                    // goal: reproduce Figure 20.5, page 461, in
                    // LeVeque (2002). "Finite Volume Methods for Hyperbolic
                    // Problems", Cambridge
                    r = PetscSqrtReal((x+0.45)*(x+0.45) + y*y);
                    if ((0.1 < x) && (x < 0.6) && (-0.25 < y) && (y < 0.25)) {
                        au[j][i] = 1.0;
                    } else if (r < 0.35) {
                        au[j][i] = 1.0 - r / 0.35;
                    }
                    break;
                default:
                    SETERRQ(PETSC_COMM_WORLD,1,"invalid user->problem\n");
            }
        }
    }
    ierr = DMDAVecRestoreArray(info->da, u, &au); CHKERRQ(ierr);
    return 0;
}

// velocity  a(x,y) = ( a^x(x,y), a^y(x,y) )
static double a_wind(double x, double y, int dir, AdvectCtx* user) {
    if (user->problem == ROTATION) {
        return (dir == 0) ? 2.0 * y : - 2.0 * x;
    } else {
        return (dir == 0) ? user->windx : user->windy;
    }
}

// source  g(x,y,u)
static double g_source(double x, double y, double u, AdvectCtx* user) {
    return 0.0;
}

//         d g(x,y,u) / d u
static double dg_source(double x, double y, double u, AdvectCtx* user) {
    return 0.0;
}

/* method-of-lines discretization gives ODE system  u' = G(t,u)
so our finite volume scheme computes
    G_ij = - (fluxE - fluxW)/hx - (fluxN - fluxS)/hy + g(x,y,U_ij)
but only east (E) and north (N) fluxes are computed
*/
//STARTFUNCTION
PetscErrorCode FormRHSFunctionLocal(DMDALocalInfo *info, double t,
        double **au, double **aG, AdvectCtx *user) {
    int         i, j, q;
    double      hx, hy, halfx, halfy, x, y, a,
                u_up, u_dn, u_far, theta, flux;

    // clear G first
    for (j = info->ys; j < info->ys + info->ym; j++)
        for (i = info->xs; i < info->xs + info->xm; i++)
            aG[j][i] = 0.0;
    // fluxes on cell boundaries are traversed in this order:  ,-1-,
    // cell center at * has coordinates (x,y):                 | * 0
    // q = 0,1 is cell boundary index                          '---'
    hx = 2.0 / info->mx;  hy = 2.0 / info->my;
    halfx = hx / 2.0;     halfy = hy / 2.0;
    for (j = info->ys-1; j < info->ys + info->ym; j++) { // note -1
        y = -1.0 + (j+0.5) * hy;
        for (i = info->xs-1; i < info->xs + info->xm; i++) {
            x = -1.0 + (i+0.5) * hx;
            if ((i >= info->xs) && (j >= info->ys)) {
                aG[j][i] += g_source(x,y,au[j][i],user);
            }
            for (q = 0; q < 2; q++) {   // get E,N fluxes on cell bdry
                if ((q == 0) && (j < info->ys))  continue;
                if ((q == 1) && (i < info->xs))  continue;
                a = a_wind(x + halfx*(1-q),y + halfy*q,q,user);
                // first-order flux
                u_up = (a >= 0.0) ? au[j][i] : au[j+q][i+(1-q)];
                flux = a * u_up;
                // use flux-limiter
                if (user->limiter != NULL) {
                    // formulas (1.2),(1.3),(1.6); H&V pp 216--217
                    u_dn = (a >= 0.0) ? au[j+q][i+(1-q)] : au[j][i];
                    if (u_dn != u_up) {
                        u_far = (a >= 0.0) ? au[j-q][i-(1-q)]
                                           : au[j+2*q][i+2*(1-q)];
                        theta = (u_up - u_far) / (u_dn - u_up);
                        flux += a * (*user->limiter)(theta)*(u_dn-u_up);
                    }
                }
                // update owned G_ij on both sides of computed flux
                if (q == 0) {
                    if (i >= info->xs)
                        aG[j][i]   -= flux / hx;
                    if (i+1 < info->xs + info->xm)
                        aG[j][i+1] += flux / hx;
                } else {
                    if (j >= info->ys)
                        aG[j][i]   -= flux / hy;
                    if (j+1 < info->ys + info->ym)
                        aG[j+1][i] += flux / hy;
                }
            }
        }
    }
    return 0;
}
//ENDFUNCTION

PetscErrorCode FormRHSJacobianLocal(DMDALocalInfo *info, double t,
        double **au, Mat J, Mat P, AdvectCtx *user) {
    PetscErrorCode ierr;
    const int   dir[4] = {0, 1, 0, 1},  // use x (0) or y (1) component
                xsh[4]   = { 1, 0,-1, 0},  ysh[4]   = { 0, 1, 0,-1};
    int         i, j, l, nc;
    double      hx, hy, halfx, halfy, x, y, a, v[5];
    MatStencil  col[5],row;

    ierr = MatZeroEntries(P); CHKERRQ(ierr);
    hx = 2.0 / info->mx;  hy = 2.0 / info->my;
    halfx = hx / 2.0;     halfy = hy / 2.0;
    for (j = info->ys; j < info->ys+info->ym; j++) {
        y = -1.0 + (j+0.5) * hy;
        row.j = j;
        for (i = info->xs; i < info->xs+info->xm; i++) {
            x = -1.0 + (i+0.5) * hx;
            row.i = i;
            col[0].j = j;  col[0].i = i;
            v[0] = dg_source(x,y,au[j][i],user);
            nc = 1;
            for (l = 0; l < 4; l++) {   // loop over cell boundaries
                a = a_wind(x + halfx*xsh[l],y + halfy*ysh[l],dir[l],user);
                switch (l) {
                    case 0:
                        col[nc].j = j;  col[nc].i = (a >= 0.0) ? i : i+1;
                        v[nc++] = - a / hx;
                        break;
                    case 1:
                        col[nc].j = (a >= 0.0) ? j : j+1;  col[nc].i = i;
                        v[nc++] = - a / hy;
                        break;
                    case 2:
                        col[nc].j = j;  col[nc].i = (a >= 0.0) ? i-1 : i;
                        v[nc++] = a / hx;
                        break;
                    case 3:
                        col[nc].j = (a >= 0.0) ? j-1 : j;  col[nc].i = i;
                        v[nc++] = a / hy;
                        break;
                }
            }
            ierr = MatSetValuesStencil(P,1,&row,nc,col,v,ADD_VALUES); CHKERRQ(ierr);
        }
    }
    ierr = MatAssemblyBegin(P,MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);
    ierr = MatAssemblyEnd(P,MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);
    if (J != P) {
        ierr = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);
        ierr = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);
    }
    return 0;
}

// dumps to file; does nothing if root is empty or NULL
PetscErrorCode dumptobinary(const char* root, const char* append, Vec u) {
    PetscErrorCode ierr;
    PetscViewer  viewer;
    char filename[PETSC_MAX_PATH_LEN] = "";
    if ((!root) || (strlen(root) == 0))  return 0;
    sprintf(filename,"%s%s.dat",root,append);
    ierr = PetscPrintf(PETSC_COMM_WORLD,
        "writing PETSC binary file %s ...\n",filename); CHKERRQ(ierr);
    ierr = PetscViewerBinaryOpen(PETSC_COMM_WORLD,filename,
        FILE_MODE_WRITE,&viewer); CHKERRQ(ierr);
    ierr = VecView(u,viewer); CHKERRQ(ierr);
    ierr = PetscViewerDestroy(&viewer); CHKERRQ(ierr);
    return 0;
}

int main(int argc,char **argv) {
    PetscErrorCode ierr;
    AdvectCtx      user;
    TS             ts;
    DM             da;
    Vec            u;
    DMDALocalInfo  info;
    LimiterType    limiterchoice = VANLEER;
    double         hx, hy, t0, dt;
    char           fileroot[PETSC_MAX_PATH_LEN] = "";

    PetscInitialize(&argc,&argv,(char*)0,help);

    user.problem = STRAIGHT;
    user.windx = 2.0;
    user.windy = 2.0;
    ierr = PetscOptionsBegin(PETSC_COMM_WORLD,
           "adv_", "options for advect.c", ""); CHKERRQ(ierr);
    ierr = PetscOptionsString("-dumpto","filename root for initial/final state",
           "advect.c",fileroot,fileroot,PETSC_MAX_PATH_LEN,NULL);CHKERRQ(ierr);
    ierr = PetscOptionsEnum("-limiter","flux-limiter type",
           "advect.c",LimiterTypes,
           (PetscEnum)limiterchoice,(PetscEnum*)&limiterchoice,NULL); CHKERRQ(ierr);
    user.limiter = limiterptr[limiterchoice];
    ierr = PetscOptionsEnum("-problem","problem type",
           "advect.c",ProblemTypes,
           (PetscEnum)user.problem,(PetscEnum*)&user.problem,NULL); CHKERRQ(ierr);
    ierr = PetscOptionsReal("-windx","x component of wind (if problem==straight)",
           "advect.c",user.windx,&user.windx,NULL);CHKERRQ(ierr);
    ierr = PetscOptionsReal("-windy","y component of wind (if problem==straight)",
           "advect.c",user.windy,&user.windy,NULL);CHKERRQ(ierr);
    ierr = PetscOptionsEnd(); CHKERRQ(ierr);

    ierr = DMDACreate2d(PETSC_COMM_WORLD,
               DM_BOUNDARY_PERIODIC, DM_BOUNDARY_PERIODIC,
               DMDA_STENCIL_STAR,              // no diagonal differencing
               5,5,PETSC_DECIDE,PETSC_DECIDE,  // default to hx=hx=0.2 grid
                                               //   (mx=my=5 allows -snes_fd_color)
               1, 2,                           // d.o.f & stencil width
               NULL,NULL,&da); CHKERRQ(ierr);
    ierr = DMSetFromOptions(da); CHKERRQ(ierr);
    ierr = DMSetUp(da); CHKERRQ(ierr);
    ierr = DMSetApplicationContext(da,&user); CHKERRQ(ierr);

    // grid is cell-centered
    ierr = DMDAGetLocalInfo(da,&info); CHKERRQ(ierr);
    hx = 2.0 / info.mx;  hy = 2.0 / info.my;
    ierr = DMDASetUniformCoordinates(da,
        -1.0+hx/2.0,1.0-hx/2.0,-1.0+hy/2.0,1.0-hy/2.0,0.0,1.0);CHKERRQ(ierr);

    ierr = TSCreate(PETSC_COMM_WORLD,&ts); CHKERRQ(ierr);
    ierr = TSSetProblemType(ts,TS_NONLINEAR); CHKERRQ(ierr);
    ierr = TSSetDM(ts,da); CHKERRQ(ierr);
    ierr = DMDATSSetRHSFunctionLocal(da,INSERT_VALUES,
           (DMDATSRHSFunctionLocal)FormRHSFunctionLocal,&user); CHKERRQ(ierr);
    ierr = DMDATSSetRHSJacobianLocal(da,
           (DMDATSRHSJacobianLocal)FormRHSJacobianLocal,&user); CHKERRQ(ierr);
    ierr = TSSetType(ts,TSRK); CHKERRQ(ierr);
    ierr = TSRKSetType(ts,TSRK2A); CHKERRQ(ierr);
    ierr = TSSetExactFinalTime(ts,TS_EXACTFINALTIME_MATCHSTEP); CHKERRQ(ierr);
    ierr = TSSetInitialTimeStep(ts,0.0,0.1); CHKERRQ(ierr);
    ierr = TSSetDuration(ts,1000000,0.6); CHKERRQ(ierr);
    ierr = TSSetFromOptions(ts);CHKERRQ(ierr);

    ierr = TSGetTime(ts,&t0); CHKERRQ(ierr);
    ierr = TSGetTimeStep(ts,&dt); CHKERRQ(ierr);
    ierr = PetscPrintf(PETSC_COMM_WORLD,
           "solving problem '%s' on %d x %d grid with dx=%g x dy=%g cells,\n"
           "  t0=%g, initial dt=%g, and '%s' limiter ...\n",
           ProblemTypes[user.problem],info.mx,info.my,hx,hy,
           t0,dt,LimiterTypes[limiterchoice]); CHKERRQ(ierr);

    ierr = DMCreateGlobalVector(da,&u); CHKERRQ(ierr);
    ierr = FormInitial(&info,u,&user); CHKERRQ(ierr);
    ierr = dumptobinary(fileroot,"_initial",u); CHKERRQ(ierr);
    ierr = TSSolve(ts,u); CHKERRQ(ierr);
    ierr = dumptobinary(fileroot,"_final",u); CHKERRQ(ierr);

    VecDestroy(&u);  TSDestroy(&ts);  DMDestroy(&da);
    PetscFinalize();
    return 0;
}
