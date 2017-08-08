#include "MPMModel.h"

using namespace dolfin;

MPMModel::MPMModel(const FunctionSpace& V,
                   const unsigned int num_dofs,
                   const Array<int>& coords,
                   double time_step,
                   bool verbosity) :
                   dofs(num_dofs), dt(time_step), verbose(verbosity)
{
  Q       = &V;
  mesh    = V.mesh();
  element = V.element();
  
  gdim    = mesh->geometry().dim();
  sdim    = element->space_dimension();
  
  printf("::: initializing MPMModelcpp with gDim = %zu,  sDim = %zu,"
         " dt = %f :::\n", gdim, sdim, dt);

  // FIXME: make the vectors of size gdim instead of 3:
  //        this will require some work with the setting of vector
  //        variables in the python code.
  h_grid.resize(dofs);     // cell diameter at node
  m_grid.resize(dofs);     // mass
  V_grid.resize(dofs);     // volume
  U3_grid.resize(3);       // velocity vector
  a3_grid.resize(3);       // acceleration vector
  U3_grid_new.resize(3);       // velocity vector
  a3_grid_new.resize(3);       // acceleration vector
  f_int_grid.resize(3);    // internal force vector
  for (unsigned int i = 0; i < 3; i++)
  {
    // set index of vector with a dimension : 
    if (coords[i] == 1)  coord_arr.push_back(i);
    U3_grid[i].resize(coords[i]*dofs);
    a3_grid[i].resize(coords[i]*dofs);
    U3_grid_new[i].resize(coords[i]*dofs);
    a3_grid_new[i].resize(coords[i]*dofs);
    f_int_grid[i].resize(coords[i]*dofs);
  }
}

void MPMModel::set_h(const Array<double>& h)
{
  for (std::size_t i = 0; i < h.size(); i++)
    h_grid[i] = h[i];
}

void MPMModel::set_V(const Array<double>& V)
{
  for (std::size_t i = 0; i < V.size(); i++)
    V_grid[i] = V[i];
}

std::vector<double>  MPMModel::get_U3(unsigned int index) const
{
  return U3_grid.at(index);
}

void  MPMModel::set_U3(unsigned int index, std::vector<double>& value)
{
  U3_grid.at(index) = value;
}

std::vector<double>  MPMModel::get_a3(unsigned int index) const
{
  return a3_grid.at(index);
}

void  MPMModel::set_a3(unsigned int index, std::vector<double>& value)
{
  a3_grid.at(index) = value;
}

std::vector<double>  MPMModel::get_f_int(unsigned int index) const
{
  return f_int_grid.at(index);
}

void  MPMModel::set_f_int(unsigned int index, std::vector<double>& value)
{
  f_int_grid.at(index) = value;
}

void MPMModel::add_material(MPMMaterial& M)
{
  materials.push_back(&M);
}

void MPMModel::set_boundary_conditions(const Array<int>& vertices,
                                       const Array<double>& values)
{
  for (unsigned int i = 0; i < vertices.size(); i++)
    bc_vrt.push_back(vertices[i]);
  for (unsigned int i = 0; i < values.size(); i++)
    bc_val.push_back(values[i]);
}

void MPMModel::update_points()
{
  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    // iterate through particle positions :
    # pragma omp parallel for schedule(auto)
    for (unsigned int j = 0; j < materials[i]->get_num_particles(); j++) 
    {
      Point* pt_j = materials[i]->get_x_pt(j);          // the particle point
      std::vector<double> x_j = materials[i]->get_x(j); // the particle coord's

      // iterate through dimension and set the coordinate :
      for (unsigned int k = 0; k < gdim; k++)
        pt_j->coordinates()[coord_arr[k]] = x_j[coord_arr[k]];
    }
  }
}

void MPMModel::update_particle_basis_functions(MPMMaterial* M)
{
  update_points();  // update the point coordinates

  // iterate through particle positions :
  for (unsigned int i = 0; i < M->get_num_particles(); i++) 
  {
    // update the grid node indices, basis values, and basis gradient values :
    Point* x_pt    = M->get_x_pt(i);
    cell_id = mesh->bounding_box_tree()->compute_first_entity_collision(*x_pt);
    cell.reset(new Cell( *mesh, cell_id));

    std::vector<double> x_i         = M->get_x(i);
    std::vector<double> phi_i       = M->get_phi(i);
    std::vector<double> grad_phi_i  = M->get_grad_phi(i);
    std::vector<unsigned int> vrt_i = M->get_vrt(i);

    cell->get_vertex_coordinates(vertex_coordinates);
    element->evaluate_basis_all(&phi_i[0],
                                &x_i[0],
                                vertex_coordinates.data(),
                                cell_orientation);
 
    element->evaluate_basis_derivatives_all(deriv_order,
                                            &grad_phi_i[0],
                                            &x_i[0],
                                            vertex_coordinates.data(),
                                            cell_orientation);

    for (unsigned int j = 0; j < Q->dofmap()->cell_dofs(0).size(); j++)
    {
      vrt_i[j] = Q->dofmap()->cell_dofs(cell->index())[j];
    }

    M->set_x(i, x_i);
    M->set_phi(i, phi_i);
    M->set_grad_phi(i, grad_phi_i);
    M->set_vrt(i, vrt_i);

  }
}

void MPMModel::formulate_material_basis_functions()
{
  if (verbose == true)
    printf("--- C++ formulate_material_basis_functions() ---\n");
  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    update_particle_basis_functions(materials[i]);
  }
}

void MPMModel::interpolate_material_mass_to_grid()
{
  if (verbose == true)
    printf("--- C++ interpolate_material_mass_to_grid() ---\n");

  // first reset the mass to zero :
  std::fill(m_grid.begin(), m_grid.end(), 0.0);

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    // iterate through particles :
    # pragma omp parallel for schedule(auto)
    for (unsigned int j = 0; j < materials[i]->get_num_particles(); j++) 
    {
      std::vector<unsigned int> idx = materials[i]->get_vrt(j);  // node index
      std::vector<double> phi_p     = materials[i]->get_phi(j);  // basis
      double m_p                    = materials[i]->get_m(j);    // mass

      // interpolate the particle mass to each node of its cell :
      for (unsigned int q = 0; q < sdim; q++)
      {
        m_grid[idx[q]] += phi_p[q] * m_p;
      }
    }
  }
}

void MPMModel::interpolate_material_velocity_to_grid()
{
  if (verbose == true)
    printf("--- C++ interpolate_material_velocity_to_grid() ---\n");

  // first reset the velocity to zero :
  for (unsigned int k = 0; k < gdim; k++)
  {
    std::fill(U3_grid[coord_arr[k]].begin(),
              U3_grid[coord_arr[k]].end(),
              0.0);
  }

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    // iterate through particles :
    # pragma omp parallel for schedule(auto)
    for (unsigned int j = 0; j < materials[i]->get_num_particles(); j++) 
    {
      std::vector<unsigned int> idx = materials[i]->get_vrt(j);  // node index
      std::vector<double> phi_p     = materials[i]->get_phi(j);  // basis
      std::vector<double> u_p       = materials[i]->get_u(j);    // velocity
      double m_p                    = materials[i]->get_m(j);    // mass

      // iterate through each coordinate :
      for (unsigned int k = 0; k < gdim; k++)
      {
        // calculate mass-conserving grid velocity at each node :
        for (unsigned int q = 0; q < sdim; q++)
        {
          U3_grid[coord_arr[k]][idx[q]] += u_p[k] * 
                                           phi_p[q] * 
                                           m_p / m_grid[idx[q]];
        }
      }
    }
  }
}

void MPMModel::calculate_grid_volume()
{
  if (verbose == true)
    printf("--- C++ calculate_grid_volume() ---\n");

  for (std::size_t i = 0; i < dofs; i++)
    V_grid[i] = 4.0/3.0 * M_PI * pow(h_grid[i]/2.0, 3);
}

  
void MPMModel::calculate_material_initial_density()
{
  printf("--- C++ calculate_material_initial_density() ---\n");

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    if (materials[i]->get_mass_init() == true)
    {
      printf("    - material `%s` has not been initialized with density -\n",
             materials[i]->get_name());
      // iterate through particles :
      for (unsigned int j = 0; j < materials[i]->get_num_particles(); j++) 
      {
        std::vector<unsigned int> idx = materials[i]->get_vrt(j);  // node index
        std::vector<double> phi_p     = materials[i]->get_phi(j);  // basis
        double m_p                    = materials[i]->get_m(j);    // mass
        double rho_j                  = 0;                         // sum ident.
      
        // interpolate the particle mass to each node of its cell :
        for (unsigned int q = 0; q < sdim; q++)
        {
          rho_j += m_grid[idx[q]] * phi_p[q] / V_grid[idx[q]];
        }
        materials[i]->set_rho0(j, rho_j);
        materials[i]->set_rho(j,  rho_j);
      }
    }
    else
    {
      printf("    - material `%s` has been initialized with density,"
             " skipping calculation -\n", materials[i]->get_name());
    }
  }
}
  
void MPMModel::calculate_material_initial_volume()
{
  printf("--- C++ calculate_material_initial_volume() ---\n");

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    if (materials[i]->get_mass_init() == true)
    {
      printf("    - material `%s` has not been initialized with volume -\n",
             materials[i]->get_name());
      materials[i]->calculate_initial_volume();
    }
    else
    {
      printf("    - material `%s` has been initialized with volume,"
             " skipping calculation -\n", materials[i]->get_name());
    }
  }
}

void MPMModel::initialize_material_tensors()
{
  if (verbose == true)
    printf("--- C++ initialize_material_tensors() ---\n");

  std::vector<double> I = materials[0]->get_I();

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    materials[i]->initialize_tensors(dt);
  }
}

void MPMModel::calculate_grid_internal_forces()
{
  if (verbose == true)
    printf("--- C++ update_grid_internal_forces() ---\n");

  unsigned int vtx, stx;

  // first reset the forces to zero :
  for (unsigned int k = 0; k < gdim; k++)
  {
    std::fill(f_int_grid[coord_arr[k]].begin(),
              f_int_grid[coord_arr[k]].end(),
              0.0);
  }

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    // iterate through particles :
    # pragma omp parallel for schedule(auto)
    for (unsigned int j = 0; j < materials[i]->get_num_particles(); j++) 
    {
      std::vector<unsigned int> idx  = materials[i]->get_vrt(j);
      std::vector<double> grad_phi_p = materials[i]->get_grad_phi(j);
      std::vector<double> sigma_p    = materials[i]->get_sigma(j);
      double V_p                     = materials[i]->get_V(j);
      stx = 0;

      // iterate through the nodes :
      for (unsigned int s = 0; s < sdim; s++)
      {
        // iterate through each component of force :
        for (unsigned int p = 0; p < gdim; p++)
        {
          vtx = p*gdim;
          // iterate through each geometric coordinate :
          for (unsigned int q = 0; q < gdim; q++)
          {
            f_int_grid[coord_arr[p]][idx[s]] -= sigma_p[vtx + q] * 
                                                grad_phi_p[stx + q] * V_p;
          }
        }
        stx += gdim;
      }
    }
  }
}

void MPMModel::calculate_grid_acceleration(double m_min)
{
  if (verbose == true)
    printf("--- C++ calculate_grid_acceleration() ---\n");
  
  // iterate through each component of acceleration :
  for (unsigned int k = 0; k < gdim; k++)
  {
    // iterate through each node :
    # pragma omp parallel for schedule(auto)
    for (unsigned int i = 0; i < dofs; i++)
    {
      // FIXME: this needs to be done properly for the first iteration too.
      // update old acceleration :
      a3_grid[coord_arr[k]][i] = a3_grid_new[coord_arr[k]][i];
    }
  }

  // iterate through each component of acceleration :
  for (unsigned int k = 0; k < gdim; k++)
  {
    // iterate through each node :
    # pragma omp parallel for schedule(auto)
    for (unsigned int i = 0; i < dofs; i++)
    {
      // if there is a mass on the grid node :
      if (m_grid[i] > 0)
      {
        // set the minimum mass :
        if (m_grid[i] < m_min)  m_grid[i] = m_min;
        // update acceleration :
        a3_grid_new[coord_arr[k]][i] = f_int_grid[coord_arr[k]][i] / m_grid[i];
      }
      // otherwise set the grid values to zero :
      else
      {
        f_int_grid[coord_arr[k]][i]  = 0.0;
        a3_grid_new[coord_arr[k]][i] = 0.0;
      }
    }
  }
}

void MPMModel::update_grid_velocity()
{
  if (verbose == true)
    printf("--- C++ update_grid_velocity() ---\n");

  // iterate through each component of velocity :
  for (unsigned int k = 0; k < gdim; k++)
  {
    // iterate through each node :
    # pragma omp parallel for schedule(auto)
    for (unsigned int i = 0; i < dofs; i++)
    {
      U3_grid_new[coord_arr[k]][i] = U3_grid[coord_arr[k]][i] +
                                     0.5 * (a3_grid[coord_arr[k]][i] + 
                                            a3_grid_new[coord_arr[k]][i]) * dt;
    }
  }
  /*
  // TODO: set boolean flag for using Dirichlet or not : 
  // apply boundary conditions if present :
  for (unsigned int k = 0; k < gdim; k++)
  {
    for (unsigned int i = 0; i < bc_val.size(); i++)
    {
      for (unsigned int j = 0; j < bc_vrt.size(); j++)
      {
        U3_grid[coord_arr[k]][bc_vrt[j]] = 0.0;
      }
    }
  }
  */
}

void MPMModel::calculate_material_velocity_gradient()
{
  if (verbose == true)
    printf("--- C++ calculate_material_velocity_gradient() ---\n");

  unsigned int vtx, stx;

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    // iterate through particles :
    for (unsigned int j = 0; j < materials[i]->get_num_particles(); j++) 
    {
      std::vector<unsigned int> idx      = materials[i]->get_vrt(j);
      std::vector<double> grad_phi_p     = materials[i]->get_grad_phi(j);
      std::vector<double> grad_u_star_p  = materials[i]->get_grad_u_star(j);
      std::vector<double> dudk_p (gdim*gdim, 0.0);
      
      // set the previous velocity gradient :
      materials[i]->set_grad_u(j, grad_u_star_p);

      // iterate through each component of velocity :
      for (unsigned int p = 0; p < gdim; p++)
      {
        // iterate through each coordinate dimension :
        for (unsigned int q = 0; q < gdim; q++)
        {
          vtx = p*gdim + q;
          stx = q;
          // iterate through each node of the particle's cell :
          for (unsigned int s = 0; s < sdim; s++)
          {
            dudk_p[vtx] += grad_phi_p[stx] * 
                           U3_grid_new[coord_arr[p]][idx[s]];
            stx += gdim;
          }
        }
      }
      materials[i]->set_grad_u_star(j, dudk_p);
    }
  } 
}

void MPMModel::update_material_deformation_gradient()
{
  if (verbose == true)
    printf("--- C++ update_material_deformation_gradient() ---\n");

  std::vector<double> I = materials[0]->get_I();

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    materials[i]->update_deformation_gradient(dt);
  }
}

void MPMModel::update_material_density()
{
  if (verbose == true)
    printf("--- C++ update_material_density() ---\n");

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    materials[i]->update_density();
  }
}

void MPMModel::update_material_volume()
{
  if (verbose == true)
    printf("--- C++ update_material_volume() ---\n");

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    materials[i]->update_volume();
  }
}

void MPMModel::update_material_stress()
{
  if (verbose == true)
    printf("--- C++ update_material_stress() ---\n");

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    materials[i]->update_stress(dt);
  }
}

void MPMModel::interpolate_grid_velocity_to_material()
{
  if (verbose == true)
    printf("--- C++ interpolate_grid_velocity_to_material() ---\n");

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    // iterate through particles :
    for (unsigned int j = 0; j < materials[i]->get_num_particles(); j++) 
    {
      std::vector<unsigned int> idx = materials[i]->get_vrt(j);  // node index
      std::vector<double> phi_p     = materials[i]->get_phi(j);  // basis
      std::vector<double> u_p (gdim, 0.0);                       // velocity

      // iterate through each velocty compoenent :
      for (unsigned int k = 0; k < gdim; k++)
      {
        // interpolate grid velocity back to particles from each node :
        for (unsigned int q = 0; q < sdim; q++)
        {
          u_p[k] += phi_p[q] * U3_grid_new[coord_arr[k]][idx[q]];
        }
      }
      materials[i]->set_u_star(j, u_p);
    }
  }
}

void MPMModel::interpolate_grid_acceleration_to_material()
{
  if (verbose == true)
    printf("--- C++ interpolate_grid_acceleration_to_material() ---\n");

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    // iterate through particles :
    for (unsigned int j = 0; j < materials[i]->get_num_particles(); j++) 
    {
      std::vector<unsigned int> idx = materials[i]->get_vrt(j);  // node index
      std::vector<double> phi_p     = materials[i]->get_phi(j);  // basis
      std::vector<double> a_star_p  = materials[i]->get_a_star(j);
      std::vector<double> a_p (gdim, 0.0);                       // acceleration
      
      // set the previous acceleration :
      materials[i]->set_a(j, a_star_p);

      // iterate through each component of velocity :
      for (unsigned int k = 0; k < gdim; k++)
      {
        // interpolate grid accleration to particles at each node q :
        for (unsigned int q = 0; q < sdim; q++)
        {
          a_p[k] += phi_p[q] * a3_grid_new[coord_arr[k]][idx[q]];
        }
      }
      materials[i]->set_a_star(j, a_p);
    }
    //materials[i]->calc_pi();
  }
}

void MPMModel::advect_material_particles()
{
  if (verbose == true)
    printf("--- C++ advect_material_particles() ---\n");

  // iterate through all materials :
  for (unsigned int i = 0; i < materials.size(); i++)
  {
    materials[i]->advect_particles(dt);
  }
}



