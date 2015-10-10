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


#include "nlp_solver_internal.hpp"
#include "mx_function.hpp"
#include "sx_function.hpp"
#include "casadi/core/timing.hpp"

INPUTSCHEME(NlpSolverInput)
OUTPUTSCHEME(NlpSolverOutput)

using namespace std;
namespace casadi {

  NlpSolverInternal::NlpSolverInternal(const std::string& name, const Function& nlp)
    : FunctionInternal(name), nlp_(nlp) {

    // Options available in all NLP solvers
    addOption("expand",             OT_BOOLEAN,  false,
              "Expand the NLP function in terms of scalar operations, i.e. MX->SX");
    addOption("hess_lag",            OT_FUNCTION,       GenericType(),
              "Function for calculating the Hessian of the Lagrangian (autogenerated by default)");
    addOption("hess_lag_options",   OT_DICT,           GenericType(),
              "Options for the autogenerated Hessian of the Lagrangian.");
    addOption("grad_lag",           OT_FUNCTION,       GenericType(),
              "Function for calculating the gradient of the Lagrangian (autogenerated by default)");
    addOption("grad_lag_options",   OT_DICT,           GenericType(),
              "Options for the autogenerated gradient of the Lagrangian.");
    addOption("jac_g",              OT_FUNCTION,       GenericType(),
              "Function for calculating the Jacobian of the constraints "
              "(autogenerated by default)");
    addOption("jac_g_options",      OT_DICT,           GenericType(),
              "Options for the autogenerated Jacobian of the constraints.");
    addOption("grad_f",             OT_FUNCTION,       GenericType(),
              "Function for calculating the gradient of the objective "
              "(column, autogenerated by default)");
    addOption("grad_f_options",     OT_DICT,           GenericType(),
              "Options for the autogenerated gradient of the objective.");
    addOption("jac_f",              OT_FUNCTION,       GenericType(),
              "Function for calculating the Jacobian of the objective "
              "(sparse row, autogenerated by default)");
    addOption("jac_f_options",     OT_DICT,           GenericType(),
              "Options for the autogenerated Jacobian of the objective.");
    addOption("iteration_callback", OT_FUNCTION, GenericType(),
              "A function that will be called at each iteration with the solver as input. "
              "Check documentation of Callback.");
    addOption("iteration_callback_step", OT_INTEGER,         1,
              "Only call the callback function every few iterations.");
    addOption("iteration_callback_ignore_errors", OT_BOOLEAN, false,
              "If set to true, errors thrown by iteration_callback will be ignored.");
    addOption("ignore_check_vec",   OT_BOOLEAN,  false,
              "If set to true, the input shape of F will not be checked.");
    addOption("warn_initial_bounds", OT_BOOLEAN,  false,
              "Warn if the initial guess does not satisfy LBX and UBX");
    addOption("eval_errors_fatal", OT_BOOLEAN, false,
              "When errors occur during evaluation of f,g,...,"
              "stop the iterations");
    addOption("verbose_init", OT_BOOLEAN, false,
              "Print out timing information about "
              "the different stages of initialization");

    addOption("defaults_recipes",    OT_STRINGVECTOR, GenericType(), "",
                                                       "qp", true);

    // Enable string notation for IO
    ischeme_ = IOScheme(SCHEME_NlpSolverInput);
    oscheme_ = IOScheme(SCHEME_NlpSolverOutput);

    // Make the ref object a non-refence counted pointer to this (as reference counting would
    // prevent deletion of the object)
    ref_.assignNodeNoCount(this);

  }

  NlpSolverInternal::~NlpSolverInternal() {
    // Explicitly remove the pointer to this (as the counter would otherwise be decreased)
    ref_.assignNodeNoCount(0);
  }

  void NlpSolverInternal::init() {
    // Initialize the NLP
    nlp_.init(false);
    casadi_assert_message(nlp_.n_in()==NL_NUM_IN,
                          "The NLP function must have exactly two input");
    casadi_assert_message(nlp_.n_out()==NL_NUM_OUT,
                          "The NLP function must have exactly two outputs");

    // Sparsity patterns
    const Sparsity& x_sparsity = nlp_.input(NL_X).sparsity();
    const Sparsity& p_sparsity = nlp_.input(NL_P).sparsity();
    const Sparsity& g_sparsity = nlp_.output(NL_G).sparsity();

    // Get dimensions
    nx_ = x_sparsity.nnz();
    np_ = p_sparsity.nnz();
    ng_ = g_sparsity.nnz();

    // Allocate space for inputs
    ibuf_.resize(NLP_SOLVER_NUM_IN);
    input(NLP_SOLVER_X0)       =  DMatrix::zeros(x_sparsity);
    input(NLP_SOLVER_LBX)      = -DMatrix::inf(x_sparsity);
    input(NLP_SOLVER_UBX)      =  DMatrix::inf(x_sparsity);
    input(NLP_SOLVER_LBG)      = -DMatrix::inf(g_sparsity);
    input(NLP_SOLVER_UBG)      =  DMatrix::inf(g_sparsity);
    input(NLP_SOLVER_LAM_X0)   =  DMatrix::zeros(x_sparsity);
    input(NLP_SOLVER_LAM_G0)   =  DMatrix::zeros(g_sparsity);
    input(NLP_SOLVER_P)        =  DMatrix::zeros(p_sparsity);

    // Allocate space for outputs
    obuf_.resize(NLP_SOLVER_NUM_OUT);
    output(NLP_SOLVER_X)       = DMatrix::zeros(x_sparsity);
    output(NLP_SOLVER_F)       = DMatrix::zeros(1);
    output(NLP_SOLVER_LAM_X)   = DMatrix::zeros(x_sparsity);
    output(NLP_SOLVER_LAM_G)   = DMatrix::zeros(g_sparsity);
    output(NLP_SOLVER_LAM_P)   = DMatrix::zeros(p_sparsity);
    output(NLP_SOLVER_G)       = DMatrix::zeros(g_sparsity);

    // Call the initialization method of the base class
    const bool verbose_init = getOption("verbose_init");
    if (verbose_init)
      userOut() << "Initializing base class...";
    const timer time0 = getTimerTime();
    FunctionInternal::init();
    const diffTime diff = diffTimers(getTimerTime(), time0);
    stats_["base class init time"] = diffToDict(diff);
    if (verbose_init)
      userOut() << "Initialized base class in " << diff.user << " seconds.";

    // Find out if we are to expand the NLP in terms of scalar operations
    bool expand = getOption("expand");
    if (expand) {
      log("Expanding NLP in scalar operations");

      // Cast to MXFunction
      MXFunction nlp_mx = shared_cast<MXFunction>(nlp_);
      if (nlp_mx.isNull()) {
        casadi_warning("Cannot expand NLP as it is not an MXFunction");
      } else {
        nlp_ = SXFunction(nlp_mx);
        nlp_.copyOptions(nlp_mx, true);
        nlp_.init();
      }
    }

    if (hasSetOption("iteration_callback")) {
      fcallback_ = getOption("iteration_callback");

      // Consistency checks
      casadi_assert(!fcallback_.isNull());
      casadi_assert(fcallback_.n_in()==NLP_SOLVER_NUM_OUT);
      casadi_assert(fcallback_.n_out()==1);
      casadi_assert(fcallback_.input(NLP_SOLVER_X).size()==x_sparsity.size());
      casadi_assert(fcallback_.input(NLP_SOLVER_F).isscalar());
      casadi_assert(fcallback_.input(NLP_SOLVER_LAM_X).size()==x_sparsity.size());
      casadi_assert(fcallback_.input(NLP_SOLVER_LAM_G).size()==g_sparsity.size());
      casadi_assert(fcallback_.input(NLP_SOLVER_LAM_P).size()==p_sparsity.size());
      casadi_assert(fcallback_.input(NLP_SOLVER_G).size()==g_sparsity.size());
    }

    callback_step_ = getOption("iteration_callback_step");
    eval_errors_fatal_ = getOption("eval_errors_fatal");

  }

  void NlpSolverInternal::checkInitialBounds() {
    const std::vector<double>& x0 = input(NLP_SOLVER_X0).data();
    const std::vector<double>& lbx = input(NLP_SOLVER_LBX).data();
    const std::vector<double>& ubx = input(NLP_SOLVER_UBX).data();
    const std::vector<double>& lbg = input(NLP_SOLVER_LBG).data();
    const std::vector<double>& ubg = input(NLP_SOLVER_UBG).data();
    const double inf = std::numeric_limits<double>::infinity();

    // Detect ill-posed problems (simple bounds)
    bool violated = false;
    for (int i=0; !violated && i<nx_; ++i)
      violated = lbx[i]==inf || lbx[i]>ubx[i] || ubx[i]==-inf;
    casadi_assert_message(!violated, "Ill-posed problem detected (x bounds)");

    // Detect ill-posed problems (nonlinear bounds)
    for (int i=0; !violated && i<ng_; ++i)
      violated = lbg[i]==inf || lbg[i]>ubg[i] || ubg[i]==-inf;
    casadi_assert_message(!violated, "Ill-posed problem detected (g bounds)");

    // Warn if initial condition violates bounds
    if (static_cast<bool>(getOption("warn_initial_bounds"))) {
      for (int k=0; !violated && k<nx_; ++k) violated = x0[k]>ubx[k] || x0[k]<lbx[k];
      if (violated) casadi_warning("NlpSolver: The initial guess does not satisfy LBX and UBX. "
                                   "Option 'warn_initial_bounds' controls this warning.");
    }
  }


  void NlpSolverInternal::reportConstraints(std::ostream &stream) {

    stream << "Reporting NLP constraints" << endl;
    FunctionInternal::reportConstraints(stream, output(NLP_SOLVER_X), input(NLP_SOLVER_LBX),
                                        input(NLP_SOLVER_UBX), "decision bounds");
    double tol = 1e-8;
    if (hasOption("constr_viol_tol")) tol = getOption("constr_viol_tol");
    FunctionInternal::reportConstraints(stream, output(NLP_SOLVER_G), input(NLP_SOLVER_LBG),
                                        input(NLP_SOLVER_UBG), "constraints", tol);
  }

  Function& NlpSolverInternal::gradF() {
    if (gradF_.isNull()) {
      gradF_ = getGradF();
    }
    return gradF_;
  }

  Function& NlpSolverInternal::jacF() {
    if (jacF_.isNull()) {
      jacF_ = getJacF();
    }
    return jacF_;
  }

  Function NlpSolverInternal::getJacF() {
    Function jacF;
    if (hasSetOption("jac_f")) {
      jacF = getOption("jac_f");
    } else {
      log("Generating objective jacobian");
      const bool verbose_init = getOption("verbose_init");
      if (verbose_init)
        userOut() << "Generating objective Jacobian...";
      const timer time0 = getTimerTime();
      jacF = nlp_.jacobian(NL_X, NL_F);
      const diffTime diff = diffTimers(getTimerTime(), time0);
      stats_["objective jacobian gen time"] = diffToDict(diff);
      if (verbose_init)
        userOut() << "Generated objective Jacobian in " << diff.user << " seconds.";
      log("Jacobian function generated");
    }
    if (hasSetOption("jac_f_options")) {
      jacF.setOption(getOption("jac_f_options"));
    }
    jacF.init(false);
    casadi_assert_message(jacF.n_in()==GRADF_NUM_IN,
                          "Wrong number of inputs to the gradient function. "
                          "Note: The gradient signature was changed in #544");
    casadi_assert_message(jacF.n_out()==GRADF_NUM_OUT,
                          "Wrong number of outputs to the gradient function. "
                          "Note: The gradient signature was changed in #544");
    jacF.setOption("input_scheme", IOScheme(SCHEME_GradFInput));
    jacF.setOption("output_scheme", IOScheme(SCHEME_GradFOutput));
    log("Objective gradient function initialized");
    return jacF;
  }

  Function NlpSolverInternal::getGradF() {
    Function gradF;
    if (hasSetOption("grad_f")) {
      gradF = getOption("grad_f");
    } else {
      log("Generating objective gradient");
      const bool verbose_init = getOption("verbose_init");
      if (verbose_init)
        userOut() << "Generating objective gradient...";
      const timer time0 = getTimerTime();
      gradF = nlp_.gradient(NL_X, NL_F);
      const diffTime diff = diffTimers(getTimerTime(), time0);
      stats_["objective gradient gen time"] = diffToDict(diff);
      if (verbose_init)
        userOut() << "Generated objective gradient in " << diff.user << " seconds.";
      log("Gradient function generated");
    }
    if (hasSetOption("grad_f_options")) {
      gradF.setOption(getOption("grad_f_options"));
    }
    gradF.init(false);
    casadi_assert_message(gradF.n_in()==GRADF_NUM_IN,
                          "Wrong number of inputs to the gradient function. "
                          "Note: The gradient signature was changed in #544");
    casadi_assert_message(gradF.n_out()==GRADF_NUM_OUT,
                          "Wrong number of outputs to the gradient function. "
                          "Note: The gradient signature was changed in #544");
    gradF.setOption("input_scheme", IOScheme(SCHEME_GradFInput));
    gradF.setOption("output_scheme", IOScheme(SCHEME_GradFOutput));
    log("Objective gradient function initialized");
    return gradF;
  }

  Function& NlpSolverInternal::jacG() {
    if (jacG_.isNull()) {
      jacG_ = getJacG();
    }
    return jacG_;
  }

  Function NlpSolverInternal::getJacG() {
    Function jacG;

    // Return null if no constraints
    if (ng_==0) return jacG;

    if (hasSetOption("jac_g")) {
      jacG = getOption("jac_g");
    } else {
      log("Generating constraint Jacobian");
      const bool verbose_init = getOption("verbose_init");
      if (verbose_init)
        userOut() << "Generating constraint Jacobian...";
      const timer time0 = getTimerTime();
      jacG = nlp_.jacobian(NL_X, NL_G);
      const diffTime diff = diffTimers(getTimerTime(), time0);
      stats_["constraint jacobian gen time"] = diffToDict(diff);
      if (verbose_init)
        userOut() << "Generated constraint Jacobian in " << diff.user << " seconds.";
      log("Jacobian function generated");
    }
    if (hasSetOption("jac_g_options")) {
      jacG.setOption(getOption("jac_g_options"));
    }
    jacG.init(false);
    casadi_assert_message(jacG.n_in()==JACG_NUM_IN,
                          "Wrong number of inputs to the Jacobian function. "
                          "Note: The Jacobian signature was changed in #544");
    casadi_assert_message(jacG.n_out()==JACG_NUM_OUT,
                          "Wrong number of outputs to the Jacobian function. "
                          "Note: The Jacobian signature was changed in #544");
    jacG.setOption("input_scheme", IOScheme(SCHEME_JacGInput));
    jacG.setOption("output_scheme", IOScheme(SCHEME_JacGOutput));
    log("Jacobian function initialized");
    return jacG;
  }

  Function& NlpSolverInternal::gradLag() {
    if (gradLag_.isNull()) {
      gradLag_ = getGradLag();
    }
    return gradLag_;
  }

  Function NlpSolverInternal::getGradLag() {
    Function gradLag;
    if (hasSetOption("grad_lag")) {
      gradLag = getOption("grad_lag");
    } else {
      log("Generating/retrieving Lagrangian gradient function");
      const bool verbose_init = getOption("verbose_init");
      if (verbose_init)
        userOut() << "Generating/retrieving Lagrangian gradient function...";
      const timer time0 = getTimerTime();
      gradLag = nlp_.derivative(0, 1);
      const diffTime diff = diffTimers(getTimerTime(), time0);
      stats_["grad lag gen time"] = diffToDict(diff);
      if (verbose_init)
        userOut() << "Generated/retrieved Lagrangien gradient in "
                  << diff.user << " seconds.";
      log("Gradient function generated");
    }
    if (hasSetOption("grad_lag_options")) {
      gradLag.setOption(getOption("grad_lag_options"));
    }
    gradLag.init(false);
    log("Gradient function initialized");
    return gradLag;
  }

  Function& NlpSolverInternal::hessLag() {
    if (hessLag_.isNull()) {
      hessLag_ = getHessLag();
    }
    return hessLag_;
  }

  Function NlpSolverInternal::getHessLag() {
    Function hessLag;
    if (hasSetOption("hess_lag")) {
      hessLag = getOption("hess_lag");
    } else {
      Function& gradLag = this->gradLag();
      log("Generating Hessian of the Lagrangian");
      const bool verbose_init = getOption("verbose_init");
      if (verbose_init)
        userOut() << "Generating Hessian of the Lagrangian...";
      const timer time0 = getTimerTime();
      hessLag = gradLag.jacobian(NL_X, NL_NUM_OUT+NL_X, false, true);
      const diffTime diff = diffTimers(getTimerTime(), time0);
      stats_["hess lag gen time"] = diffToDict(diff);
      if (verbose_init)
        userOut() << "Generated Hessian of the Lagrangian in "
                  << diff.user << " seconds.";
      log("Hessian function generated");
    }
    if (hasSetOption("hess_lag_options")) {
      hessLag.setOption(getOption("hess_lag_options"));
    }
    hessLag.init(false);
    casadi_assert_message(hessLag.n_in()==HESSLAG_NUM_IN,
                          "Wrong number of inputs to the Hessian function. "
                          "Note: The Lagrangian Hessian signature was changed in #544");
    casadi_assert_message(hessLag.n_out()==HESSLAG_NUM_OUT,
                          "Wrong number of outputs to the Hessian function. "
                          "Note: The Lagrangian Hessian signature was changed in #544");
    hessLag.setOption("input_scheme", IOScheme(SCHEME_HessLagInput));
    hessLag.setOption("output_scheme", IOScheme(SCHEME_HessLagOutput));

    log("Hessian function initialized");
    return hessLag;
  }

  Sparsity& NlpSolverInternal::spHessLag() {
    if (spHessLag_.isNull()) {
      spHessLag_ = getSpHessLag();
    }
    return spHessLag_;
  }

  Sparsity NlpSolverInternal::getSpHessLag() {
    Sparsity spHessLag;
    if (false /*hasSetOption("hess_lag_sparsity")*/) {
      // NOTE: No such option yet, need support for GenericType(Sparsity)
      //spHessLag = getOption("hess_lag_sparsity");
    } else {
      Function& gradLag = this->gradLag();
      log("Generating Hessian of the Lagrangian sparsity pattern");
      const bool verbose_init = getOption("verbose_init");
      if (verbose_init)
        userOut() << "Generating Hessian of the Lagrangian sparsity pattern...";
      const timer time0 = getTimerTime();
      spHessLag = gradLag.jacSparsity(NL_X, NL_NUM_OUT+NL_X, false, true);
      const diffTime diff = diffTimers(getTimerTime(), time0);
      stats_["hess lag sparsity time"] = diffToDict(diff);
      if (verbose_init)
        userOut() << "Generated Hessian of the Lagrangian sparsity pattern in "
                  << diff.user << " seconds.";
      log("Hessian sparsity pattern generated");
    }
    return spHessLag;
  }

  void NlpSolverInternal::checkInputs() const {
    for (int i=0;i<input(NLP_SOLVER_LBX).nnz();++i) {
      casadi_assert_message(input(NLP_SOLVER_LBX).at(i)<=input(NLP_SOLVER_UBX).at(i),
                            "LBX[i] <= UBX[i] was violated for i=" << i
                            << ". Got LBX[i]=" << input(NLP_SOLVER_LBX).at(i)
                            << " and UBX[i]=" << input(NLP_SOLVER_UBX).at(i));
    }
    for (int i=0;i<input(NLP_SOLVER_LBG).nnz();++i) {
      casadi_assert_message(input(NLP_SOLVER_LBG).at(i)<=input(NLP_SOLVER_UBG).at(i),
                            "LBG[i] <= UBG[i] was violated for i=" << i
                            << ". Got LBG[i]=" << input(NLP_SOLVER_LBG).at(i)
                            << " and UBG[i]=" << input(NLP_SOLVER_UBG).at(i));
    }
  }

  std::map<std::string, NlpSolverInternal::Plugin> NlpSolverInternal::solvers_;

  const std::string NlpSolverInternal::infix_ = "nlpsolver";

  DMatrix NlpSolverInternal::getReducedHessian() {
    casadi_error("NlpSolverInternal::getReducedHessian not defined for class "
                 << typeid(*this).name());
    return DMatrix();
  }

  void NlpSolverInternal::setOptionsFromFile(const std::string & file) {
    casadi_error("NlpSolverInternal::setOptionsFromFile not defined for class "
                 << typeid(*this).name());
  }

  double NlpSolverInternal::defaultInput(int ind) const {
    switch (ind) {
    case NLP_SOLVER_LBX:
    case NLP_SOLVER_LBG:
      return -std::numeric_limits<double>::infinity();
    case NLP_SOLVER_UBX:
    case NLP_SOLVER_UBG:
      return std::numeric_limits<double>::infinity();
    default:
      return 0;
    }
  }

} // namespace casadi
