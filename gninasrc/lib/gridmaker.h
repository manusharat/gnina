/*
 * gridmaker.h
 *
 * Fills in grids with atom type information.  Implemented within the header
 * to make it easier to include as a dependency (cuda code separate though).
 *  Created on: May 31, 2016
 *      Author: dkoes
 */

#ifndef _GRIDMAKER_H_
#define _GRIDMAKER_H_

#include <vector>
#include <cmath>
#include <cuda.h>
#include <stdarg.h>
#include <device_types.h>
#include <thrust/system/cuda/experimental/pinned_allocator.h>
#include <vector_types.h>
#include <boost/array.hpp>
#include <boost/multi_array/multi_array_ref.hpp>
#include <boost/math/quaternion.hpp>
#include <boost/algorithm/string.hpp>
#include "quaternion.h"
#include "gridoptions.h"
#include "caffe/proto/caffe.pb.h"

#ifndef VINA_ATOM_CONSTANTS_H
#include "gninasrc/lib/atom_constants.h"
#endif
#include "gpu_math.h"

using namespace std;

template<typename Dtype, typename GridMakerT>
__global__
void set_atom_gradients(GridMakerT gmaker, const float4* ainfo, short* gridindices, 
    float3* agrads, float3 centroid, const qt Q, float3 translation, const Dtype* grids, 
    unsigned remainder_offset, bool isrelevance=false);

template<typename Grids, typename GridMakerT, typename quaternion>
void set_atom_cpu(float4 ainfo, int whichgrid, const quaternion& Q, 
    Grids& grids, GridMakerT& gmaker);

template<typename Grids, typename GridMakerT, typename quaternion>
void set_atom_gradient_cpu(const float4& ainfo, int whichgrid,
    const quaternion& Q, const Grids& grids,
    float3& agrad, GridMakerT& gmaker, const Grids& densegrids=Grids(NULL, vector<typename Grids::element>(Grids::dimensionality, 0)), bool isrelevance = false);

class GridMaker {
  protected:
    float2 dims[3];
    float3 center;
    float radiusmultiple;
    float resolution;
    float dimension;
    float rsq; //radius squared (dimension/2)^2
    unsigned dim; //number of points on each side (grid)
    bool binary;
    bool spherize; //mask out atoms not within sphere of center
  
  public:
    template<typename Grids, typename GridMakerT, typename quaternion> friend 
      void set_atom_cpu(float4 ainfo, int whichgrid, const quaternion& Q, 
      Grids& grids, GridMakerT& gmaker);

    template<typename Grids, typename GridMakerT, typename quaternion> friend 
      void set_atom_gradient_cpu(const float4& ainfo, int whichgrid,
      const quaternion& Q, const Grids& grids,
      float3& agrad, GridMakerT& gmaker, const Grids& densegrids, bool isrelevance);

    template<typename Dtype, typename GridMakerT> __global__ friend void 
      set_atom_gradients(GridMakerT gmaker, const float4* ainfo, short* gridindices,
      float3* agrads, float3 centroid, const qt Q, float3 translation, const Dtype* grids, 
      unsigned remainder_offset, bool isrelevance);
  
    typedef boost::math::quaternion<float> quaternion;

    GridMaker(float res = 0, float d = 0, float rm = 1.5, bool b = false,
        bool s = false)
        : radiusmultiple(rm), resolution(res), dimension(d), binary(b),
            spherize(s) {
      initialize(res,d,rm,b,s);
    }
  
    virtual ~GridMaker() {}
  
    virtual void initialize(float res, float d, float rm = 1.5, bool b = false, bool s =
        false) {
      resolution = res;
      dimension = d;
      radiusmultiple = rm;
      binary = b;
      spherize = s;
      dim = ::round(dimension / resolution) + 1; //number of grid points on a side
      rsq = (dimension / 2) * (dimension / 2);
      center.x = center.y = center.z = 0;
    }
  
    virtual void initialize(const caffe::MolGridDataParameter& param) {
      initialize(param.resolution(), param.dimension(), param.radius_multiple(), 
          param.binary_occupancy(), param.spherical_mask());
    }
  
    virtual void initialize(const gridoptions& opt, float rm) {
      initialize(opt.res, opt.dim, rm, opt.binary, opt.spherize);
    }

    __host__ __device__ float get_resolution() {return resolution;}

    __host__ __device__ float get_radiusmultiple() {return radiusmultiple;}

    //mus set center before gridding
    virtual void setCenter(double x, double y, double z) {
      center.x = x;
      center.y = y;
      center.z = z;
      float half = dimension/2.0;
      dims[0].x = x - half;
      dims[0].y = x + half;
      dims[1].x = y - half;
      dims[1].y = y + half;
      dims[2].x = z - half;
      dims[2].y = z + half;
    }
  
    template<typename Grid>
    void zeroGridsCPU(vector<Grid>& grids) {
      for (unsigned i = 0, n = grids.size(); i < n; i++) {
        std::fill(grids[i].data(), grids[i].data() + grids[i].num_elements(), 0.0);
      }
    }
    
    template<typename Grids>
    void zeroGridsCPU(Grids& grids) {
      std::fill(grids.data(), grids.data() + grids.num_elements(), 0.0);
    }

    pair<unsigned, unsigned> getrange(const float2& d, double c, double r) {
      pair<unsigned, unsigned> ret(0, 0);
      double low = c - r - d.x;
      if (low > 0) {
        ret.first = floor(low / resolution);
      }

      double high = c + r - d.x;
      if (high > 0) //otherwise zero
          {
        ret.second = std::min(dim, (unsigned) ceil(high / resolution));
      }
      return ret;
    }

    __device__ uint2 getrange_gpu(const float2& d, double c, double r) {
      uint2 ret = make_uint2(0, 0);
      double low = c - r - d.x;

      if (low > 0) {
        ret.x = floor(low / resolution);
      }

      double high = c + r - d.x;
      if (high > 0) //otherwise zero
          {
        ret.y = min(dim, (unsigned) ceil(high / resolution));
      }
      return ret;
    }

    //return the occupancy for atom a at point x,y,z
    __host__ __device__
    float calcPoint(const float3& coords, double ar, float x, float y,
        float z) {
      float dx = x - coords.x;
      float dy = y - coords.y;
      float dz = z - coords.z;

      float rsq = dx * dx + dy * dy + dz * dz;
      if (binary) {
        //is point within radius?
        if (rsq < ar * ar)
          return 1.0;
        else
          return 0.0;
      } else {
        //for non binary we want a gaussian were 2 std occurs at the radius
        //after which which switch to a quadratic
        //the quadratic is to fit to have both the same value and first order
        //derivative at the cross over point and a value and derivative of zero
        //at 1.5*radius
        double dist = sqrt(rsq);
        if (dist >= ar * radiusmultiple) {
          return 0.0;
        } else
          if (dist <= ar) {
            //return gaussian
            float h = 0.5 * ar;
            float ex = -dist * dist / (2 * h * h);
            return exp(ex);
          } else //return quadratic
          {
            float h = 0.5 * ar;
            float eval = 1.0 / (M_E * M_E); //e^(-2)
            float q = dist * dist * eval / (h * h) - 6.0 * eval * dist / h
                + 9.0 * eval;
            return q;
          }
      }
      //this was here, not sure why, giving "statement unreachable" compiler
      //warning and you know how much I love placating the compiler
      //return 0.0;
    }

    template <typename Dtype>
    void setAtomsCPU(const vector<float4>& ainfo, const vector<short>& gridindex,  
        const quaternion& Q, Dtype* data, unsigned ntypes) {
      boost::multi_array_ref<Dtype, 4> grids(data, boost::extents[ntypes][dim][dim][dim]);
      setAtomsCPU(ainfo, gridindex, Q, grids);
    }
  
    //Grids is either a vector of multi_arrays or a multi_array
    //set the relevant grid points for passed info
    template<typename Grids>
    void setAtomsCPU(const vector<float4>& ainfo, const vector<short>& gridindex,  
        const quaternion& Q, Grids& grids) {
      zeroGridsCPU(grids);
      for (unsigned i = 0, n = ainfo.size(); i < n; i++) {
        int pos = gridindex[i];
        if (pos >= 0)
          set_atom_cpu(ainfo[i], pos, Q, grids, *this);
      }
    }
  
    template<bool Binary, typename Dtype> __device__ 
    void set_atoms(float3 origin, unsigned n, float4 *ainfos, short *gridindex, 
        Dtype *grids);
  
    //GPU accelerated version, defined in cu file
    //pointers must point to GPU memory
    template<typename Dtype>
    void setAtomsGPU(unsigned natoms, float4 *coords, short *gridindex, qt Q, 
        unsigned ngrids, Dtype *grids);
  
    virtual void zeroGridsStartBatchGPU(float* grids, unsigned ngrids);
    virtual void zeroGridsStartBatchGPU(double* grids, unsigned ngrids);
  
    void zeroAtomGradientsCPU(vector<float3>& agrad) {
      for (unsigned i = 0, n = agrad.size(); i < n; ++i) { 
        agrad[i].x = 0.0;
        agrad[i].y = 0.0;
        agrad[i].z = 0.0;
      }
    }

    __host__ __device__
    void accumulateAtomRelevance(const float3& coords, double ar, float x,
        float y, float z, float gridval, float denseval, float3& agrad) {
      //simple sum of values that the atom overlaps
      float dist_x = x - coords.x;
      float dist_y = y - coords.y;
      float dist_z = z - coords.z;
      float dist2 = dist_x * dist_x + dist_y * dist_y + dist_z * dist_z;
      double dist = sqrt(dist2);
      if (dist >= ar * radiusmultiple) {
        return;
      } else {
        if(denseval > 0) {
          //weight by contribution to density grid
          float val = calcPoint(coords, ar, x, y, z);
          agrad.x += gridval*val/denseval;
        } else {
          agrad.x += gridval;
        }
      }
    }

    //accumulate gradient from grid point x,y,z for provided atom
    __host__ __device__
    void accumulateAtomGradient(const float3& coords, double ar, float x,
              float y, float z, float gridval, float3& agrad, int whichgrid) {
      //sum gradient grid values overlapped by the atom times the
      //derivative of the atom density at each grid point
      float dist_x = x - coords.x;
      float dist_y = y - coords.y;
      float dist_z = z - coords.z;
      float dist2 = dist_x * dist_x + dist_y * dist_y + dist_z * dist_z;
      double dist = sqrt(dist2);
      float agrad_dist = 0.0;
      if (dist >= ar * radiusmultiple) {//no overlap
        return;
      }
      else if (dist <= ar) {//gaussian derivative
        float h = 0.5 * ar;
        float ex = -dist2 / (2 * h * h);
        float coef = -dist / (h * h);
        agrad_dist = coef * exp(ex);
      }
      else {//quadratic derivative
        float h = 0.5 * ar;
        float inv_e2 = 1.0 / (M_E * M_E); //e^(-2)
        agrad_dist = 2.0 * dist * inv_e2 / (h * h) - 6.0 * inv_e2 / h;
      }
      // d_loss/d_atomx = d_atomdist/d_atomx * d_gridpoint/d_atomdist * d_loss/d_gridpoint
      // sum across all gridpoints
      //dkoes - the negative sign is because we are considering the derivative of the center vs grid
      float gx = -(dist_x / dist) * agrad_dist * gridval;
      float gy = -(dist_y / dist) * agrad_dist * gridval;
      float gz = -(dist_z / dist) * agrad_dist * gridval;
      agrad.x += gx;
      agrad.y += gy;
      agrad.z += gz;
    }
  
    template <typename Dtype>
    void setAtomGradientsCPU(const vector<float4>& ainfo, const vector<short>& gridindex, 
                             quaternion Q, Dtype* data, vector<float3>& agrad, 
                             unsigned offset, unsigned ntypes) {
      boost::multi_array_ref<Dtype, 4> grids(data+offset, boost::extents[ntypes][dim][dim][dim]);
      setAtomGradientsCPU(ainfo, gridindex, Q, grids, agrad);
    }
  
    //backpropagate the gradient from atom grid to atom x,y,z positions
    template<typename Grids>
    void setAtomGradientsCPU(const vector<float4>& ainfo, 
        const vector<short>& gridindex, const quaternion& Q, const Grids& grids, 
        vector<float3>& agrad) { 
      zeroAtomGradientsCPU(agrad);
      for (unsigned i = 0, n = ainfo.size(); i < n; ++i) {
        int whichgrid = gridindex[i]; // this is which atom-type channel of the grid to look at
        if (whichgrid >= 0) {
          set_atom_gradient_cpu(ainfo[i], whichgrid, Q, grids, agrad[i], *this);
        }
      }
    }

    template <typename Grids>
    void setVal(unsigned i, unsigned j, unsigned k, int whichgrid, float val, Grids& grids) {
      grids[whichgrid][i][j][k] = val;
    }

    template <typename Grids>
    void addVal(unsigned i, unsigned j, unsigned k, int whichgrid, float val, Grids& grids) {
      grids[whichgrid][i][j][k] += val;
    }

    __host__ __device__
    int getIndexFromPoint(unsigned i, unsigned j, unsigned k, int whichgrid);

    template <typename Grids>
    auto getGridElement(Grids& grids, int whichgrid, unsigned i, unsigned j, unsigned k) ->
      decltype(&grids[0][0][0][0])
    {
      return &grids[whichgrid][i][j][k];
    }

    //summ up gradient values overlapping atoms
    template<typename Grids>
    void setAtomRelevanceCPU(const vector<float4>& ainfo,
        const vector<short>& gridindex, const quaternion& Q, const Grids& densegrids,
        const Grids& diffgrids, vector<float3>& agrad) {
      zeroAtomGradientsCPU(agrad);
      for (unsigned i = 0, n = ainfo.size(); i < n; ++i) {
        int whichgrid = gridindex[i]; // this is which atom-type channel of the grid to look at
        if (whichgrid >= 0) {
          set_atom_gradient_cpu(ainfo[i], whichgrid, Q, diffgrids, agrad[i], *this, 
              densegrids, true);
        }
      }
    }

    virtual unsigned createDefaultMap(const char *names[], vector<int>& map) {
      map.assign(smina_atom_type::NumTypes, -1);
      const char **nameptr = names;
      unsigned cnt = 0;
      while (*nameptr != NULL) {
        string line(*nameptr);
        vector<string> names;
        boost::algorithm::split(names, line, boost::is_space(), 
            boost::algorithm::token_compress_on);
        for(unsigned i = 0, n = names.size(); i < n; i++) {
          string name = names[i];
          smt t = string_to_smina_type(name);
          if(t < smina_atom_type::NumTypes) //valid
          {
            map[t] = cnt;
          }
          else //should never happen
          {
            cerr << "Invalid atom type " << name << "\n";
            exit(-1);
          }
        }

        if(names.size()) //skip empty lines
          cnt++;

        nameptr++;
      }
      return cnt;
    }

    //create atom mapping from whitespace/newline delimited string
    static unsigned createMapFromString(const std::string& rmap, vector<int>& map) {
      map.assign(smina_atom_type::NumTypes, -1);

      //split string into lines
      vector<string> lines;
      boost::algorithm::split(lines, rmap, boost::is_any_of("\n"),boost::algorithm::token_compress_on);
      unsigned cnt = 0;
      for (auto line : lines) {
        vector<string> names;

        //split line into distinct types
        boost::algorithm::split(names, line, boost::is_space(),
            boost::algorithm::token_compress_on);
        for (unsigned i = 0, n = names.size(); i < n; i++) {
          string name = names[i];
          smt t = string_to_smina_type(name);
          if (t < smina_atom_type::NumTypes) { //valid
            map[t] = cnt;
          } else {//should never happen
            cerr << "Invalid atom type " << name << "\n";
            exit(-1);
          }
        }

        if(names.size()) cnt++;
      }
      return cnt;
    }

    //initialize default receptor/ligand maps
    //these were determined by an analysis of type frequencies
    virtual unsigned createDefaultRecMap(vector<int>& map) {
      const char *names[] =
      { "AliphaticCarbonXSHydrophobe",
          "AliphaticCarbonXSNonHydrophobe",
          "AromaticCarbonXSHydrophobe",
          "AromaticCarbonXSNonHydrophobe",
          "Calcium",
          "Iron",
          "Magnesium",
          "Nitrogen",
          "NitrogenXSAcceptor",
          "NitrogenXSDonor",
          "NitrogenXSDonorAcceptor",
          "OxygenXSAcceptor",
          "OxygenXSDonorAcceptor",
          "Phosphorus",
          "Sulfur",
          "Zinc", NULL };

      return createDefaultMap(names, map);
    }

    virtual unsigned createDefaultLigMap(vector<int>& map) {
      const char *names[] =
      { "AliphaticCarbonXSHydrophobe",
          "AliphaticCarbonXSNonHydrophobe",
          "AromaticCarbonXSHydrophobe",
          "AromaticCarbonXSNonHydrophobe",
          "Bromine",
          "Chlorine",
          "Fluorine",
          "Nitrogen",
          "NitrogenXSAcceptor",
          "NitrogenXSDonor",
          "NitrogenXSDonorAcceptor",
          "Oxygen",
          "OxygenXSAcceptor",
          "OxygenXSDonorAcceptor",
          "Phosphorus",
          "Sulfur",
          "SulfurAcceptor",
          "Iodine",
          "Boron",
          NULL };
      return createDefaultMap(names, map);
    }

    //create a mapping from atom type ids to a unique id given a file specifying
    //what types we care about (anything missing is ignored); if multiple types are
    //on the same line, they are merged, if the file isn't specified, use default mapping
    //return total number of types
    //map is indexed by smina_atom_type, maps to -1 if type should be ignored
    virtual unsigned createAtomTypeMap(const string& fname, vector<int>& map) {
      using namespace std;
      using namespace boost::algorithm;
      map.assign(smina_atom_type::NumTypes, -1);

      if(fname.size() == 0) {
        std::cerr <<  "Map file not specified\n";
        exit(-1);
      }

      unsigned cnt = 0;
      ifstream in(fname.c_str());

      if(!in) {
        std::cerr << "Could not open " << fname << "\n";
        exit(-1);
      }

      string line;
      while (getline(in, line)) {
        vector<string> types;
        split(types, line, is_any_of("\t \n"));
        for (unsigned i = 0, n = types.size(); i < n; i++) {
          const string& name = types[i];
          smt t = string_to_smina_type(name);
          if (t < smina_atom_type::NumTypes) //valid
          {
            map[t] = cnt;
          }
          else if (name.size() > 0) //this ignores consecutive delimiters
          {
            cerr << "Invalid atom type " << name << "\n";
            exit(-1);
          }
        }
        if (types.size() > 0)
          cnt++;
      }
      return cnt;
    }

};  

class SubcubeGridMaker : public GridMaker {
  public:
    float subgrid_dim;
    unsigned batch_size;
    unsigned stride;
    unsigned batch_idx;
    unsigned ntypes;
    unsigned nrec_types;
    unsigned nlig_types;
    unsigned subgrid_dim_in_points;
    unsigned grids_per_dim;
    SubcubeGridMaker(float res=0, float d=0, float rm=1.5, bool b=false, 
        bool s=false, float sd=0.0, unsigned bs=1, unsigned strd=0, unsigned bi=0, 
        unsigned nt=0, unsigned nrt=0, unsigned nlt=0, unsigned sdip=0, 
        unsigned gpd=0) : 
      GridMaker(res, d, rm, b, s), subgrid_dim(sd), batch_size(bs), stride(strd), 
      batch_idx(bi), ntypes(nt), nrec_types(nrt), nlig_types(nlt), subgrid_dim_in_points(sdip),
      grids_per_dim(gpd) {
      initialize(res, d, rm, b, s, sd, bs, strd, bi, nt, nrt, nlt, sdip, gpd);
    }

    template<typename Grids, typename GridMakerT, typename quaternion> friend 
      void set_atom_cpu(float4 ainfo, int whichgrid, const quaternion& Q, 
      Grids& grids, GridMakerT& gmaker);

    template<typename Grids, typename GridMakerT, typename quaternion> friend 
      void set_atom_gradient_cpu(const float4& ainfo, int whichgrid,
      const quaternion& Q, const Grids& grids,
      float3& agrad, GridMakerT& gmaker, const Grids& densegrids, bool isrelevance);

    virtual ~SubcubeGridMaker() {}

    virtual void initialize(float res, float d, float rm = 1.5, bool b = false, bool s =
        false) {
      std::cout << "WARNING: Using subcube GridMaker but subgrid dimension not provided\n";
      subgrid_dim = 0;
      batch_size = 1;
      stride = 0;
      batch_idx = 0;
      ntypes = 0;
      nrec_types = 0;
      nlig_types = 0;
      subgrid_dim_in_points = 0;
      grids_per_dim = 0;
      GridMaker::initialize(res, d, rm, b, s);
    }

    virtual void initialize(float res, float d, float rm=1.5, bool b = false, 
        bool s = false, float sd=0.0, unsigned bs=1, unsigned strd=0, unsigned bi=0, 
        unsigned nt=0, unsigned nrt=0, unsigned nlt=0, unsigned sdip=0, unsigned gpd=0) {
      subgrid_dim = sd;
      batch_size = bs;
      stride = strd;
      batch_idx = bi;
      ntypes = nt;
      nrec_types = nrt;
      nlig_types = nlt;
      subgrid_dim_in_points = ::round(subgrid_dim / res) + 1;
      subgrid_dim = res * (subgrid_dim_in_points - 1);
      //if the subgrid decomposition doesn't cover at least dim, increase dim
      //so that it does
      grids_per_dim = ::round((d - subgrid_dim) / (subgrid_dim + res)) + 1;
      d = (subgrid_dim + res) * (grids_per_dim - 1) + subgrid_dim;
      unsigned dim_pts = ::round(d / res) + 1;
      if (stride)
        grids_per_dim = ((dim_pts - subgrid_dim_in_points) / stride) + 1;
      GridMaker::initialize(res, d, rm, b, s);
    }

    virtual void initialize(const caffe::MolGridDataParameter& param) {
      initialize(param.resolution(), param.dimension(), param.radius_multiple(), 
          param.binary_occupancy(), param.spherical_mask(), param.subgrid_dim(), 
          param.batch_size(), param.stride());
    }

    virtual void initialize(const gridoptions& opt, float rm) {
      initialize(opt.res, opt.dim, rm, opt.binary, opt.spherize, opt.subgrid_dim);
    }

    virtual unsigned createDefaultMap(const char *names[], vector<int>& map) {
      unsigned _ntypes = GridMaker::createDefaultMap(names, map);
      ntypes += _ntypes;
      return _ntypes;
    }

    virtual unsigned createDefaultRecMap(vector<int>& map) {
      nrec_types = GridMaker::createDefaultRecMap(map);
      return nrec_types;
    }

    virtual unsigned createDefaultLigMap(vector<int>& map) {
      nlig_types = GridMaker::createDefaultLigMap(map);
      return nlig_types;
    }

    virtual unsigned createAtomTypeMap(const string& fname, vector<int>& map) {
      unsigned _ntypes = GridMaker::createAtomTypeMap(fname, map);
      ntypes += _ntypes;
      return _ntypes;
    }

    __host__ __device__
    void getRelativeIndices(unsigned i, unsigned j, unsigned k, 
        unsigned& rel_x, unsigned& rel_y, unsigned& rel_z, unsigned& grid_idx) {
      unsigned subgrid_idx_x = i / subgrid_dim_in_points; 
      unsigned subgrid_idx_y = j / subgrid_dim_in_points; 
      unsigned subgrid_idx_z = k / subgrid_dim_in_points; 
      rel_x = i % subgrid_dim_in_points; 
      rel_y = j % subgrid_dim_in_points; 
      rel_z = k % subgrid_dim_in_points; 
      grid_idx = (((subgrid_idx_x * grids_per_dim) + 
            subgrid_idx_y) * grids_per_dim + subgrid_idx_z);
    }

    template <typename Dtype>
    void setAtomsCPU(const vector<float4>& ainfo, const vector<short>& gridindex, 
        const quaternion& Q, Dtype* data, unsigned ntypes) {
      if (stride) {
        GridMaker::setAtomsCPU(ainfo, gridindex, Q, data, ntypes);
      }
      else {
        unsigned ngrids = grids_per_dim * grids_per_dim * grids_per_dim;
        boost::multi_array_ref<Dtype, 6> grids(data, 
            boost::extents[ngrids][batch_size][ntypes][subgrid_dim_in_points]
            [subgrid_dim_in_points][subgrid_dim_in_points]);
        setAtomsCPU(ainfo, gridindex, Q, grids);
      }
    }

    template<typename Grids>
    void setAtomsCPU(const vector<float4>& ainfo, 
      const vector<short>& gridindex,  const quaternion& Q, Grids& grids) {
      if (batch_idx == 0)
        zeroGridsCPU(grids);
      for (unsigned i = 0, n = ainfo.size(); i < n; i++) {
        int pos = gridindex[i];
        if (pos >= 0)
          set_atom_cpu(ainfo[i], pos, Q, grids, *this);
      }
      batch_idx = (batch_idx + 1) % batch_size;
    }

    template<typename Grids>
    auto getGridElement(Grids& grids, unsigned whichgrid, unsigned i, unsigned j, unsigned k) -> 
        decltype(&grids[0][0][0][0][0][0]){
      unsigned rel_x; 
      unsigned rel_y; 
      unsigned rel_z; 
      unsigned grid_idx;
      getRelativeIndices(i, j, k, rel_x, rel_y, rel_z, grid_idx);
      return &grids[grid_idx][batch_idx][whichgrid][rel_x][rel_y][rel_z];
    }

    template<typename Allocator, typename Dtype>
    Dtype* getGridElement(std::vector<boost::multi_array<Dtype, 3, 
        Allocator>>& grids, unsigned whichgrid, unsigned i, unsigned j, unsigned k) {
      if (stride) {
        Dtype* ret = GridMaker::getGridElement(grids, i, j, k, whichgrid);
        return ret;
      }
      else {
        unsigned rel_x; 
        unsigned rel_y; 
        unsigned rel_z; 
        unsigned grid_idx;
        getRelativeIndices(i, j, k, rel_x, rel_y, rel_z, grid_idx);
        return &grids[grid_idx * ntypes + whichgrid][rel_x][rel_y][rel_z];
      }
    }

    template<typename Dtype>
    Dtype* getGridElement(boost::multi_array_ref<Dtype, 4>& grids, 
        unsigned whichgrid, unsigned i, unsigned j, unsigned k) {
      if (stride) {
        Dtype* ret = GridMaker::getGridElement(grids, i, j, k, whichgrid);
        return ret;
      }
      else {
        unsigned rel_x; 
        unsigned rel_y; 
        unsigned rel_z; 
        unsigned grid_idx;
        getRelativeIndices(i, j, k, rel_x, rel_y, rel_z, grid_idx);
        return &grids[grid_idx * ntypes + whichgrid][rel_x][rel_y][rel_z];
      }
    }

    template<bool Binary, typename Dtype> __device__ 
    void set_atoms(float3 origin, unsigned n, float4 *ainfos, short *gridindex, 
        Dtype *grids);

    //defined in cu. N.B. this _cannot_ be virtual because we are passing the
    //gridmaker to a GPU kernel by value and the vtable is not copied
    template<typename Dtype>
    void setAtomsGPU(unsigned natoms, float4 *ainfos, short *gridindex, 
        qt Q, unsigned ngrids, Dtype *grids);

    virtual void zeroGridsStartBatchGPU(float* grids, unsigned ngrids);
    virtual void zeroGridsStartBatchGPU(double* grids, unsigned ngrids);

    template <typename Dtype>
    void setAtomGradientsCPU(const vector<float4>& ainfo, 
        const vector<short>& gridindex, quaternion Q, Dtype* data, 
        vector<float3>& agrad, unsigned offset, unsigned ntypes) {
      if (stride) {
        GridMaker::setAtomGradientsCPU(ainfo, gridindex, Q, data, agrad, offset, ntypes);
      }
      else {
        unsigned ngrids = grids_per_dim * grids_per_dim * grids_per_dim;
        boost::multi_array_ref<Dtype, 6> grids(data, boost::extents[ngrids]
            [batch_size][ntypes][subgrid_dim_in_points][subgrid_dim_in_points]
            [subgrid_dim_in_points]);
        setAtomGradientsCPU(ainfo, gridindex, Q, grids, agrad);
      }
    }

    template<typename Grids>
    void setAtomGradientsCPU(const vector<float4>& ainfo, 
        const vector<short>& gridindex, const quaternion& Q, 
        const Grids& grids, vector<float3>& agrad) {
      zeroAtomGradientsCPU(agrad);
      for (unsigned i = 0, n = ainfo.size(); i < n; ++i) {
        int whichgrid = gridindex[i]; // this is which atom-type channel of the grid to look at
        if (whichgrid >= 0) {
          set_atom_gradient_cpu(ainfo[i], whichgrid, Q, grids, agrad[i], *this);
        }
      }
    }

    __host__ __device__
    int getIndexFromPoint(unsigned i, unsigned j, unsigned k, int whichgrid);

    template <typename Grids>
    void setVal(unsigned i, unsigned j, unsigned k, int whichgrid, float val, Grids& grids) {
      *(getGridElement(grids, whichgrid, i, j, k)) = val;
    }

    template <typename Grids>
    void addVal(unsigned i, unsigned j, unsigned k, int whichgrid, float val, Grids& grids) {
      *(getGridElement(grids, whichgrid, i, j, k)) += val;
    }
};

//set the relevant grid points for provided atom
template<typename Grids, typename GridMakerT, typename quaternion>
void set_atom_cpu(float4 ainfo, int whichgrid, const quaternion& Q, 
    Grids& grids, GridMakerT& gmaker) {
  float radius = ainfo.w;
  float r = radius * gmaker.radiusmultiple;
  float3 coords;

  if(gmaker.spherize) {
    float xdiff = ainfo.x - gmaker.center.x;
    float ydiff = ainfo.y - gmaker.center.y;
    float zdiff = ainfo.z - gmaker.center.z;
    float distsq = xdiff * xdiff + ydiff * ydiff + zdiff * zdiff;
    if(distsq > gmaker.rsq) {
      return; //ignore
    }
  }

  if (Q.real() != 0) { //apply rotation
    quaternion p(0, ainfo.x - gmaker.center.x, ainfo.y - gmaker.center.y, ainfo.z - gmaker.center.z);
    p = Q * p * (conj(Q) / norm(Q));
    coords.x = p.R_component_2() + gmaker.center.x;
    coords.y = p.R_component_3() + gmaker.center.y;
    coords.z = p.R_component_4() + gmaker.center.z;
  }
  else {
    coords.x = ainfo.x;
    coords.y = ainfo.y;
    coords.z = ainfo.z;
  }

  vector<pair<unsigned, unsigned> > ranges(3);
  ranges[0] = gmaker.getrange(gmaker.dims[0], coords.x, r);
  ranges[1] = gmaker.getrange(gmaker.dims[1], coords.y, r);
  ranges[2] = gmaker.getrange(gmaker.dims[2], coords.z, r);

  //for every grid point possibly overlapped by this atom
  for (unsigned i = ranges[0].first, iend = ranges[0].second; i < iend; i++) {
    for (unsigned j = ranges[1].first, jend = ranges[1].second; j < jend;
        j++) {
      for (unsigned k = ranges[2].first, kend = ranges[2].second;
          k < kend; k++) {
        float x = gmaker.dims[0].x + i * gmaker.resolution;
        float y = gmaker.dims[1].x + j * gmaker.resolution;
        float z = gmaker.dims[2].x + k * gmaker.resolution;
        float val = gmaker.calcPoint(coords, radius, x, y, z);

        if (gmaker.binary) {
          if (val != 0) 
            gmaker.setVal(i, j, k, whichgrid, 1.0, grids);
        }
        else {
          gmaker.addVal(i, j, k, whichgrid, val, grids);
        }

      }
    }
  }
}
  
//get the atom position gradient from relevant grid points for provided atom
//if isrelevance is true, simply sum overlapping values
template<typename Grids, typename GridMakerT, typename quaternion>
void set_atom_gradient_cpu(const float4& ainfo, int whichgrid,
    const quaternion& Q, const Grids& grids,
    float3& agrad, GridMakerT& gmaker, const Grids& densegrids, bool isrelevance) {
  float3 coords;
  if (Q.real() != 0) {//apply rotation
    quaternion p(0, ainfo.x - gmaker.center.x, ainfo.y - gmaker.center.y,
        ainfo.z - gmaker.center.z);
    p = Q * p * (conj(Q) / norm(Q));

    coords.x = p.R_component_2() + gmaker.center.x;
    coords.y = p.R_component_3() + gmaker.center.y;
    coords.z = p.R_component_4() + gmaker.center.z;
  } 
  else {
    coords.x = ainfo.x;
    coords.y = ainfo.y;
    coords.z = ainfo.z;
  }

  //get grid index ranges that could possibly be overlapped by atom
  float radius = ainfo.w;
  float r = radius * gmaker.radiusmultiple;
  vector<pair<unsigned, unsigned> > ranges(3);
  ranges[0] = gmaker.getrange(gmaker.dims[0], coords.x, r);
  ranges[1] = gmaker.getrange(gmaker.dims[1], coords.y, r);
  ranges[2] = gmaker.getrange(gmaker.dims[2], coords.z, r);

  //for every grid point possibly overlapped by this atom
  for (unsigned i = ranges[0].first, iend = ranges[0].second; i < iend;
      ++i) {
    for (unsigned j = ranges[1].first, jend = ranges[1].second;
        j < jend; ++j) {
      for (unsigned k = ranges[2].first, kend = ranges[2].second;
          k < kend; ++k) {
        //convert grid point coordinates to angstroms
        float x = gmaker.dims[0].x + i * gmaker.resolution;
        float y = gmaker.dims[1].x + j * gmaker.resolution;
        float z = gmaker.dims[2].x + k * gmaker.resolution;

        if (isrelevance) 
          gmaker.accumulateAtomRelevance(coords, radius, x, y, z,
              *(gmaker.getGridElement(grids, whichgrid, i, j, k)), 
              *(gmaker.getGridElement(densegrids, whichgrid, i, j, k)), agrad);
        else //true gradient, distance matters
          gmaker.accumulateAtomGradient(coords, radius, x, y, z,
              *(gmaker.getGridElement(grids, whichgrid, i, j, k)), agrad, whichgrid);
      }
    }
  }
}
  
#endif /* _GRIDMAKER_H_ */
