#ifndef __MPMIMPENETRABLEMATERIAL_H
#define __MPMIMPENETRABLEMATERIAL_H

#include "MPMMaterial.h"

namespace dolfin
{
  class MPMImpenetrableMaterial : public MPMMaterial
  {
    public:
      MPMImpenetrableMaterial(const Array<double>& m_a,
                              const Array<double>& x_a,
                              const Array<double>& u_a,
                              const FiniteElement& element);
     ~MPMImpenetrableMaterial() {};

      void calculate_stress() {};
      void calculate_strain_rate() {};
      void calculate_incremental_strain_rate() {};

  };
}
#endif
