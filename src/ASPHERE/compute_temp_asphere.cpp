/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Mike Brown (SNL)
------------------------------------------------------------------------- */

#include "mpi.h"
#include "string.h"
#include "compute_temp_asphere.h"
#include "math_extra.h"
#include "atom.h"
#include "atom_vec_ellipsoid.h"
#include "update.h"
#include "force.h"
#include "domain.h"
#include "modify.h"
#include "fix.h"
#include "group.h"
#include "memory.h"
#include "error.h"

using namespace LAMMPS_NS;

/* ---------------------------------------------------------------------- */

ComputeTempAsphere::ComputeTempAsphere(LAMMPS *lmp, int narg, char **arg) :
  Compute(lmp, narg, arg)
{
  if (narg != 3 && narg != 4)
    error->all("Illegal compute temp/asphere command");

  scalar_flag = vector_flag = 1;
  size_vector = 6;
  extscalar = 0;
  extvector = 1;
  tempflag = 1;

  tempbias = 0;
  id_bias = NULL;
  if (narg == 4) {
    tempbias = 1;
    int n = strlen(arg[3]) + 1;
    id_bias = new char[n];
    strcpy(id_bias,arg[3]);
  }

  vector = new double[6];

  // error check

  avec = (AtomVecEllipsoid *) atom->style_match("ellipsoid");
  if (!avec) 
    error->all("Compute temp/asphere requires atom style ellipsoid");
}

/* ---------------------------------------------------------------------- */

ComputeTempAsphere::~ComputeTempAsphere()
{
  delete [] id_bias;
  delete [] vector;
}

/* ---------------------------------------------------------------------- */

void ComputeTempAsphere::init()
{
  // check that all particles are finite-size
  // no point particles allowed, spherical is OK

  int *ellipsoid = atom->ellipsoid;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  for (int i = 0; i < nlocal; i++)
    if (mask[i] & groupbit)
      if (ellipsoid[i] < 0)
	error->one("Compute temp/asphere requires extended particles");

  if (tempbias) {
    int i = modify->find_compute(id_bias);
    if (i < 0) error->all("Could not find compute ID for temperature bias");
    tbias = modify->compute[i];
    if (tbias->tempflag == 0)
      error->all("Bias compute does not calculate temperature");
    if (tbias->tempbias == 0)
      error->all("Bias compute does not calculate a velocity bias");
    if (tbias->igroup != igroup)
      error->all("Bias compute group does not match compute group");
    tbias->init();
    if (strcmp(tbias->style,"temp/region") == 0) tempbias = 2;
    else tempbias = 1;
  }

  fix_dof = 0;
  for (int i = 0; i < modify->nfix; i++)
    fix_dof += modify->fix[i]->dof(igroup);
  dof_compute();
}

/* ---------------------------------------------------------------------- */

void ComputeTempAsphere::dof_compute()
{
  // 6 dof for 3d, 3 dof for 2d
  // assume full rotation of extended particles
  // user should correct this via compute_modify if needed

  double natoms = group->count(igroup);
  int nper = 6;
  if (domain->dimension == 2) nper = 3;
  dof = nper*natoms;

  // additional adjustments to dof

  if (tempbias == 1) dof -= tbias->dof_remove(-1) * natoms;
  else if (tempbias == 2) {
    int *mask = atom->mask;
    int nlocal = atom->nlocal;
    int count = 0;
    for (int i = 0; i < nlocal; i++)
      if (mask[i] & groupbit)
	if (tbias->dof_remove(i)) count++;
    int count_all;
    MPI_Allreduce(&count,&count_all,1,MPI_INT,MPI_SUM,world);
    dof -= nper*count_all;
  }

  dof -= extra_dof + fix_dof;
  if (dof > 0) tfactor = force->mvv2e / (dof * force->boltz);
  else tfactor = 0.0;
}

/* ---------------------------------------------------------------------- */

double ComputeTempAsphere::compute_scalar()
{
  invoked_scalar = update->ntimestep;

  if (tempbias) {
    if (tbias->invoked_scalar != update->ntimestep) tbias->compute_scalar();
    tbias->remove_bias_all();
  }

  AtomVecEllipsoid::Bonus *bonus = avec->bonus;
  int *ellipsoid = atom->ellipsoid;
  double **v = atom->v;
  double **angmom = atom->angmom;
  double *rmass = atom->rmass;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  double *shape,*quat;
  double wbody[3],inertia[3];
  double rot[3][3];
  double t = 0.0;
  
  // sum translationals and rotational energy for each particle
  // no point particles since divide by inertia

  for (int i = 0; i < nlocal; i++)
    if (mask[i] & groupbit) {

      shape = bonus[ellipsoid[i]].shape;
      quat = bonus[ellipsoid[i]].quat;

      t += (v[i][0]*v[i][0] + v[i][1]*v[i][1] + v[i][2]*v[i][2]) * rmass[i];

      // principal moments of inertia

      inertia[0] = rmass[i] * (shape[1]*shape[1]+shape[2]*shape[2]) / 5.0;
      inertia[1] = rmass[i] * (shape[0]*shape[0]+shape[2]*shape[2]) / 5.0;
      inertia[2] = rmass[i] * (shape[0]*shape[0]+shape[1]*shape[1]) / 5.0;

      // wbody = angular velocity in body frame
      
      MathExtra::quat_to_mat(quat,rot);
      MathExtra::transpose_matvec(rot,angmom[i],wbody);
      wbody[0] /= inertia[0];
      wbody[1] /= inertia[1];
      wbody[2] /= inertia[2];

      t += inertia[0]*wbody[0]*wbody[0] +
	inertia[1]*wbody[1]*wbody[1] + inertia[2]*wbody[2]*wbody[2];
    }

  if (tempbias) tbias->restore_bias_all();

  MPI_Allreduce(&t,&scalar,1,MPI_DOUBLE,MPI_SUM,world);
  if (dynamic || tempbias == 2) dof_compute();
  scalar *= tfactor;
  return scalar;
}

/* ---------------------------------------------------------------------- */

void ComputeTempAsphere::compute_vector()
{
  int i;

  invoked_vector = update->ntimestep;

  if (tempbias) {
    if (tbias->invoked_vector != update->ntimestep) tbias->compute_vector();
    tbias->remove_bias_all();
  }

  AtomVecEllipsoid::Bonus *bonus = avec->bonus;
  int *ellipsoid = atom->ellipsoid;
  double **v = atom->v;
  double **angmom = atom->angmom;
  double *rmass = atom->rmass;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  double *shape,*quat;
  double wbody[3],inertia[3];
  double rot[3][3];
  double massone,t[6];
  for (i = 0; i < 6; i++) t[i] = 0.0;

  for (i = 0; i < nlocal; i++)
    if (mask[i] & groupbit) {

      shape = bonus[ellipsoid[i]].shape;
      quat = bonus[ellipsoid[i]].quat;

      // translational kinetic energy

      massone = rmass[i];
      t[0] += massone * v[i][0]*v[i][0];
      t[1] += massone * v[i][1]*v[i][1];
      t[2] += massone * v[i][2]*v[i][2];
      t[3] += massone * v[i][0]*v[i][1];
      t[4] += massone * v[i][0]*v[i][2];
      t[5] += massone * v[i][1]*v[i][2];

      // principal moments of inertia

      inertia[0] = rmass[i] * (shape[1]*shape[1]+shape[2]*shape[2]) / 5.0;
      inertia[1] = rmass[i] * (shape[0]*shape[0]+shape[2]*shape[2]) / 5.0;
      inertia[2] = rmass[i] * (shape[0]*shape[0]+shape[1]*shape[1]) / 5.0;

      // wbody = angular velocity in body frame

      MathExtra::quat_to_mat(quat,rot);
      MathExtra::transpose_matvec(rot,angmom[i],wbody);
      wbody[0] /= inertia[0];
      wbody[1] /= inertia[1];
      wbody[2] /= inertia[2];

      // rotational kinetic energy

      t[0] += inertia[0]*wbody[0]*wbody[0];
      t[1] += inertia[1]*wbody[1]*wbody[1];
      t[2] += inertia[2]*wbody[2]*wbody[2];
      t[3] += inertia[0]*wbody[0]*wbody[1];
      t[4] += inertia[1]*wbody[0]*wbody[2];
      t[5] += inertia[2]*wbody[1]*wbody[2];
    }

  if (tempbias) tbias->restore_bias_all();

  MPI_Allreduce(t,vector,6,MPI_DOUBLE,MPI_SUM,world);
  for (i = 0; i < 6; i++) vector[i] *= force->mvv2e;
}

/* ----------------------------------------------------------------------
   remove velocity bias from atom I to leave thermal velocity
------------------------------------------------------------------------- */

void ComputeTempAsphere::remove_bias(int i, double *v)
{
  if (tbias) tbias->remove_bias(i,v);
}

/* ----------------------------------------------------------------------
   add back in velocity bias to atom I removed by remove_bias()
   assume remove_bias() was previously called
------------------------------------------------------------------------- */

void ComputeTempAsphere::restore_bias(int i, double *v)
{
  if (tbias) tbias->restore_bias(i,v);
}
