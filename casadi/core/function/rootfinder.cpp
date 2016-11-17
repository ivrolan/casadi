/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "rootfinder_impl.hpp"
#include "../mx/mx_node.hpp"
#include <iterator>
#include "linsol.hpp"

#include "../global_options.hpp"

using namespace std;
namespace casadi {

  bool has_rootfinder(const string& name) {
    return Rootfinder::has_plugin(name);
  }

  void load_rootfinder(const string& name) {
    Rootfinder::load_plugin(name);
  }

  string doc_rootfinder(const string& name) {
    return Rootfinder::getPlugin(name).doc;
  }

Function Function::rootfinder_fun() {
    casadi_assert(!is_null());
    Rootfinder* n = dynamic_cast<Rootfinder*>(get());
    casadi_assert_message(n!=0, "Not a rootfinder");
    return n->oracle();
  }

  Function rootfinder(const std::string& name, const std::string& solver,
                   const Function& f, const Dict& opts) {
    Function ret;
    ret.assignNode(Rootfinder::instantiatePlugin(name, solver, f));
    ret->construct(opts);
    return ret;
  }

  Rootfinder::Rootfinder(const std::string& name, const Function& oracle)
    : OracleFunction(name, oracle) {

    // Default options
    iin_ = 0;
    iout_ = 0;
  }

  Rootfinder::~Rootfinder() {
  }

  Options Rootfinder::options_
  = {{&OracleFunction::options_},
     {{"linear_solver",
       {OT_STRING,
        "User-defined linear solver class. Needed for sensitivities."}},
      {"linear_solver_options",
       {OT_DICT,
        "Options to be passed to the linear solver."}},
      {"constraints",
       {OT_INTVECTOR,
        "Constrain the unknowns. 0 (default): no constraint on ui, "
        "1: ui >= 0.0, -1: ui <= 0.0, 2: ui > 0.0, -2: ui < 0.0."}},
      {"implicit_input",
       {OT_INT,
        "Index of the input that corresponds to the actual root-finding"}},
      {"implicit_output",
       {OT_INT,
        "Index of the output that corresponds to the actual root-finding"}},
      {"jacobian_function",
       {OT_FUNCTION,
        "Function object for calculating the Jacobian (autogenerated by default)"}}
     }
  };

  void Rootfinder::init(const Dict& opts) {

    // Default (temporary) options
    Dict linear_solver_options;
    string linear_solver = "csparse";
    Function jac; // Jacobian of f with respect to z

    // Read options
    for (auto&& op : opts) {
      if (op.first=="implicit_input") {
        iin_ = op.second;
      } else if (op.first=="implicit_output") {
        iout_ = op.second;
      } else if (op.first=="jacobian_function") {
        jac = op.second;
      } else if (op.first=="linear_solver_options") {
        linear_solver_options = op.second;
      } else if (op.first=="linear_solver") {
        linear_solver = op.second.to_string();
      } else if (op.first=="constraints") {
        u_c_ = op.second;
      }
    }

    // Get the number of equations and check consistency
    casadi_assert_message(iin_>=0 && iin_<oracle_.n_in() && oracle_.n_in()>0,
                          "Implicit input not in range");
    casadi_assert_message(iout_>=0 && iout_<oracle_.n_out() && oracle_.n_out()>0,
                          "Implicit output not in range");
    casadi_assert_message(oracle_.sparsity_out(iout_).is_dense()
                          && oracle_.sparsity_out(iout_).is_column(),
                          "Residual must be a dense vector");
    casadi_assert_message(oracle_.sparsity_in(iin_).is_dense()
                          && oracle_.sparsity_in(iin_).is_column(),
                          "Unknown must be a dense vector");
    n_ = oracle_.nnz_out(iout_);
    casadi_assert_message(n_ == oracle_.nnz_in(iin_),
                          "Dimension mismatch. Input size is "
                          << oracle_.nnz_in(iin_)
                          << ", while output size is "
                          << oracle_.nnz_out(iout_));

    // Call the base class initializer
    OracleFunction::init(opts);

    // Generate Jacobian if not provided
    if (jac.is_null()) jac = oracle_.jacobian(iin_, iout_);
    set_function(jac, "jac_f_z");
    sp_jac_ = jac.sparsity_out(0);

    // Check for structural singularity in the Jacobian
    casadi_assert_message(!sp_jac_.is_singular(),
      "Rootfinder::init: singularity - the jacobian is structurally rank-deficient. "
      "sprank(J)=" << sprank(sp_jac_) << " (instead of "<< sp_jac_.size1() << ")");

    // Get the linear solver creator function
    linsol_ = Linsol("linsol", linear_solver, linear_solver_options);

    // Constraints
    casadi_assert_message(u_c_.size()==n_ || u_c_.empty(),
                          "Constraint vector if supplied, must be of length n, but got "
                          << u_c_.size() << " and n = " << n_);

    // Allocate sufficiently large work vectors
    alloc(oracle_);
    size_t sz_w = oracle_.sz_w();
    if (!jac.is_null()) {
      sz_w = max(sz_w, jac.sz_w());
    }
    alloc_w(sz_w + 2*static_cast<size_t>(n_));
  }

  void Rootfinder::init_memory(void* mem) const {
    OracleFunction::init_memory(mem);
    //auto m = static_cast<RootfinderMemory*>(mem);
    linsol_.reset(sp_jac_);
  }

  void Rootfinder::eval(void* mem, const double** arg, double** res, int* iw, double* w) const {
    // Reset the solver, prepare for solution
    setup(mem, arg, res, iw, w);

    // Solve the NLP
    solve(mem);
  }

  void Rootfinder::set_work(void* mem, const double**& arg, double**& res,
                        int*& iw, double*& w) const {
    auto m = static_cast<RootfinderMemory*>(mem);

    // Get input pointers
    m->iarg = arg;
    arg += n_in();

    // Get output pointers
    m->ires = res;
    res += n_out();
  }

  Function Rootfinder
  ::get_forward(const std::string& name, int nfwd,
                const std::vector<std::string>& i_names,
                const std::vector<std::string>& o_names, const Dict& opts) {
    // Symbolic expression for the input
    vector<MX> arg = mx_in();
    arg[iin_] = MX::sym(arg[iin_].name() + "_guess",
                        Sparsity(arg[iin_].size()));
    vector<MX> res = mx_out();
    vector<vector<MX> > fseed = symbolicFwdSeed(nfwd, arg), fsens;
    forward(arg, res, fseed, fsens, false, false);

    // Construct return function
    arg.insert(arg.end(), res.begin(), res.end());
    vector<MX> v(nfwd);
    for (int i=0; i<n_in(); ++i) {
      for (int d=0; d<nfwd; ++d) v[d] = fseed[d][i];
      arg.push_back(horzcat(v));
    }
    res.clear();
    for (int i=0; i<n_out(); ++i) {
      for (int d=0; d<nfwd; ++d) v[d] = fsens[d][i];
      res.push_back(horzcat(v));
    }
    return Function(name, arg, res, i_names, o_names, opts);
  }

  Function Rootfinder
  ::get_reverse(const std::string& name, int nadj,
                const std::vector<std::string>& i_names,
                const std::vector<std::string>& o_names, const Dict& opts) {
    // Symbolic expression for the input
    vector<MX> arg = mx_in();
    arg[iin_] = MX::sym(arg[iin_].name() + "_guess",
                        Sparsity(arg[iin_].size()));
    vector<MX> res = mx_out();
    vector<vector<MX> > aseed = symbolicAdjSeed(nadj, res), asens;
    reverse(arg, res, aseed, asens, false, false);

    // Construct return function
    arg.insert(arg.end(), res.begin(), res.end());
    vector<MX> v(nadj);
    for (int i=0; i<n_out(); ++i) {
      for (int d=0; d<nadj; ++d) v[d] = aseed[d][i];
      arg.push_back(horzcat(v));
    }
    res.clear();
    for (int i=0; i<n_in(); ++i) {
      for (int d=0; d<nadj; ++d) v[d] = asens[d][i];
      res.push_back(horzcat(v));
    }
    return Function(name, arg, res, i_names, o_names, opts);
  }

  void Rootfinder::sp_fwd(const bvec_t** arg, bvec_t** res, int* iw, bvec_t* w, int mem) const {
    int num_out = n_out();
    int num_in = n_in();
    bvec_t* tmp1 = w; w += n_;
    bvec_t* tmp2 = w; w += n_;

    // Propagate dependencies through the function
    const bvec_t** arg1 = arg+n_in();
    copy(arg, arg+num_in, arg1);
    arg1[iin_] = 0;
    bvec_t** res1 = res+n_out();
    fill_n(res1, num_out, static_cast<bvec_t*>(0));
    res1[iout_] = tmp1;
    oracle_(arg1, res1, iw, w, 0);

    // "Solve" in order to propagate to z
    fill_n(tmp2, n_, 0);
    sp_jac_.spsolve(tmp2, tmp1, false);
    if (res[iout_]) copy(tmp2, tmp2+n_, res[iout_]);

    // Propagate to auxiliary outputs
    if (num_out>1) {
      arg1[iin_] = tmp2;
      copy(res, res+num_out, res1);
      res1[iout_] = 0;
      oracle_(arg1, res1, iw, w, 0);
    }
  }

  void Rootfinder::sp_rev(bvec_t** arg, bvec_t** res, int* iw, bvec_t* w, int mem) const {
    int num_out = n_out();
    int num_in = n_in();
    bvec_t* tmp1 = w; w += n_;
    bvec_t* tmp2 = w; w += n_;

    // Get & clear seed corresponding to implicitly defined variable
    if (res[iout_]) {
      copy(res[iout_], res[iout_]+n_, tmp1);
      fill_n(res[iout_], n_, 0);
    } else {
      fill_n(tmp1, n_, 0);
    }

    // Propagate dependencies from auxiliary outputs to z
    bvec_t** res1 = res+num_out;
    copy(res, res+num_out, res1);
    res1[iout_] = 0;
    bvec_t** arg1 = arg+num_in;
    copy(arg, arg+num_in, arg1);
    arg1[iin_] = tmp1;
    if (num_out>1) {
      oracle_.rev(arg1, res1, iw, w, 0);
    }

    // "Solve" in order to get seed
    fill_n(tmp2, n_, 0);
    sp_jac_.spsolve(tmp2, tmp1, true);

    // Propagate dependencies through the function
    for (int i=0; i<num_out; ++i) res1[i] = 0;
    res1[iout_] = tmp2;
    arg1[iin_] = 0; // just a guess
    oracle_.rev(arg1, res1, iw, w, 0);
  }

  std::map<std::string, Rootfinder::Plugin> Rootfinder::solvers_;

  const std::string Rootfinder::infix_ = "rootfinder";

  void Rootfinder::
  forward(const std::vector<MX>& arg, const std::vector<MX>& res,
          const std::vector<std::vector<MX> >& fseed,
          std::vector<std::vector<MX> >& fsens,
          bool always_inline, bool never_inline) {
    // Number of directional derivatives
    int nfwd = fseed.size();
    fsens.resize(nfwd);

    // Quick return if no seeds
    if (nfwd==0) return;

    // Propagate through f_
    vector<MX> f_arg(arg);
    f_arg.at(iin_) = res.at(iout_);
    vector<MX> f_res(res);
    f_res.at(iout_) = MX(size_in(iin_)); // zero residual
    std::vector<std::vector<MX> > f_fseed(fseed);
    for (int d=0; d<nfwd; ++d) {
      f_fseed[d].at(iin_) = MX(size_in(iin_)); // ignore seeds for guess
    }
    oracle_->forward(f_arg, f_res, f_fseed, fsens, always_inline, never_inline);

    // Get expression of Jacobian
    Function jac = get_function("jac_f_z");
    MX J = jac(f_arg).front();

    // Solve for all the forward derivatives at once
    vector<MX> rhs(nfwd);
    for (int d=0; d<nfwd; ++d) rhs[d] = vec(fsens[d][iout_]);
    rhs = horzsplit(J->getSolve(-horzcat(rhs), false, linsol_));
    for (int d=0; d<nfwd; ++d) fsens[d][iout_] = reshape(rhs[d], size_in(iin_));

    // Propagate to auxiliary outputs
    int num_out = n_out();
    if (num_out>1) {
      for (int d=0; d<nfwd; ++d) f_fseed[d][iin_] = fsens[d][iout_];
      oracle_->forward(f_arg, f_res, f_fseed, fsens, always_inline, never_inline);
      for (int d=0; d<nfwd; ++d) fsens[d][iout_] = f_fseed[d][iin_]; // Otherwise overwritten
    }
  }

  void Rootfinder::
  reverse(const std::vector<MX>& arg, const std::vector<MX>& res,
          const std::vector<std::vector<MX> >& aseed,
          std::vector<std::vector<MX> >& asens,
          bool always_inline, bool never_inline) {

    // Number of directional derivatives
    int nadj = aseed.size();
    asens.resize(nadj);

    // Quick return if no seeds
    if (nadj==0) return;

    // Get expression of Jacobian
    vector<MX> f_arg(arg);
    f_arg[iin_] = res.at(iout_);
    Function jac = get_function("jac_f_z");
    MX J = jac(f_arg).front();

    // Get adjoint seeds for calling f
    int num_out = n_out();
    int num_in = n_in();
    vector<MX> f_res(res);
    f_res[iout_] = MX(size_in(iin_)); // zero residual
    vector<vector<MX> > f_aseed(nadj);
    for (int d=0; d<nadj; ++d) {
      f_aseed[d].resize(num_out);
      for (int i=0; i<num_out; ++i) f_aseed[d][i] = i==iout_ ? f_res[iout_] : aseed[d][i];
    }

    // Propagate dependencies from auxiliary outputs
    vector<MX> rhs(nadj);
    vector<vector<MX> > asens_aux;
    if (num_out>1) {
      oracle_->reverse(f_arg, f_res, f_aseed, asens_aux, always_inline, never_inline);
      for (int d=0; d<nadj; ++d) rhs[d] = vec(asens_aux[d][iin_] + aseed[d][iout_]);
    } else {
      for (int d=0; d<nadj; ++d) rhs[d] = vec(aseed[d][iout_]);
    }

    // Solve for all the adjoint seeds at once
    rhs = horzsplit(J->getSolve(-horzcat(rhs), true, linsol_));
    for (int d=0; d<nadj; ++d) {
      for (int i=0; i<num_out; ++i) {
        if (i==iout_) {
          f_aseed[d][i] = reshape(rhs[d], size_out(i));
        } else {
          // Avoid counting the auxiliary seeds twice
          f_aseed[d][i] = MX(size_out(i));
        }
      }
    }

    // No dependency on guess (1)
    vector<MX> tmp(nadj);
    for (int d=0; d<nadj; ++d) {
      asens[d].resize(num_in);
      tmp[d] = asens[d][iin_].is_empty(true) ? MX(size_in(iin_)) : asens[d][iin_];
    }

    // Propagate through f_
    oracle_->reverse(f_arg, f_res, f_aseed, asens, always_inline, never_inline);

    // No dependency on guess (2)
    for (int d=0; d<nadj; ++d) {
      asens[d][iin_] = tmp[d];
    }

    // Add contribution from auxiliary outputs
    if (num_out>1) {
      for (int d=0; d<nadj; ++d) {
        for (int i=0; i<num_in; ++i) if (i!=iin_) asens[d][i] += asens_aux[d][i];
      }
    }
  }

} // namespace casadi
