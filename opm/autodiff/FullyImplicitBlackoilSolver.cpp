/*
  Copyright 2013 SINTEF ICT, Applied Mathematics.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <opm/autodiff/FullyImplicitBlackoilSolver.hpp>


#include <opm/autodiff/AutoDiffBlock.hpp>
#include <opm/autodiff/AutoDiffHelpers.hpp>
#include <opm/autodiff/BlackoilPropsAdInterface.hpp>
#include <opm/autodiff/GeoProps.hpp>

#include <opm/core/grid.h>
#include <opm/core/linalg/LinearSolverInterface.hpp>
#include <opm/core/props/rock/RockCompressibility.hpp>
#include <opm/core/simulator/BlackoilState.hpp>
#include <opm/core/simulator/WellState.hpp>
#include <opm/core/utility/ErrorMacros.hpp>

#include <cassert>
#include <cmath>
#include <iostream>
#include <iomanip>

// A debugging utility.
#define DUMP(foo)                                                       \
    do {                                                                \
        std::cout << "==========================================\n"     \
                  << #foo ":\n"                                         \
                  << collapseJacs(foo) << std::endl;                    \
    } while (0)



namespace Opm {

typedef AutoDiffBlock<double> ADB;
typedef ADB::V V;
typedef ADB::M M;
typedef Eigen::Array<double,
                     Eigen::Dynamic,
                     Eigen::Dynamic,
                     Eigen::RowMajor> DataBlock;


namespace {


    std::vector<int>
    buildAllCells(const int nc)
    {
        std::vector<int> all_cells(nc);

        for (int c = 0; c < nc; ++c) { all_cells[c] = c; }

        return all_cells;
    }

    template <class GeoProps>
    AutoDiffBlock<double>::M
    gravityOperator(const UnstructuredGrid& grid,
                    const HelperOps&        ops ,
                    const GeoProps&         geo )
    {
        const int nc = grid.number_of_cells;

        std::vector<int> f2hf(2 * grid.number_of_faces, -1);
        for (int c = 0, i = 0; c < nc; ++c) {
            for (; i < grid.cell_facepos[c + 1]; ++i) {
                const int f = grid.cell_faces[ i ];
                const int p = 0 + (grid.face_cells[2*f + 0] != c);

                f2hf[2*f + p] = i;
            }
        }

        typedef AutoDiffBlock<double>::V V;
        typedef AutoDiffBlock<double>::M M;

        const V& gpot  = geo.gravityPotential();
        const V& trans = geo.transmissibility();

        const HelperOps::IFaces::Index ni = ops.internal_faces.size();

        typedef Eigen::Triplet<double> Tri;
        std::vector<Tri> grav;  grav.reserve(2 * ni);
        for (HelperOps::IFaces::Index i = 0; i < ni; ++i) {
            const int f  = ops.internal_faces[ i ];
            const int c1 = grid.face_cells[2*f + 0];
            const int c2 = grid.face_cells[2*f + 1];

            assert ((c1 >= 0) && (c2 >= 0));

            const double dG1 = gpot[ f2hf[2*f + 0] ];
            const double dG2 = gpot[ f2hf[2*f + 1] ];
            const double t   = trans[ f ];

            grav.push_back(Tri(i, c1,   t * dG1));
            grav.push_back(Tri(i, c2, - t * dG2));
        }

        M G(ni, nc);  G.setFromTriplets(grav.begin(), grav.end());

        return G;
    }



    V computePerfPress(const UnstructuredGrid& grid, const Wells& wells, const V& rho, const double grav)
    {
        const int nw = wells.number_of_wells;
        const int nperf = wells.well_connpos[nw];
        const int dim = grid.dimensions;
        V wdp = V::Zero(nperf,1);
        assert(wdp.size() == rho.size());

        // Main loop, iterate over all perforations,
        // using the following formula:
        //    wdp(perf) = g*(perf_z - well_ref_z)*rho(perf)
        // where the total density rho(perf) is taken to be
        //    sum_p (rho_p*saturation_p) in the perforation cell.
        // [although this is computed on the outside of this function].
        for (int w = 0; w < nw; ++w) {
            const double ref_depth = wells.depth_ref[w];
            for (int j = wells.well_connpos[w]; j < wells.well_connpos[w + 1]; ++j) {
                const int cell = wells.well_cells[j];
                const double cell_depth = grid.cell_centroids[dim * cell + dim - 1];
                wdp[j] = rho[j]*grav*(cell_depth - ref_depth);
            }
        }
        return wdp;
    }



    template <class PU>
    std::vector<bool>
    activePhases(const PU& pu)
    {
        const int maxnp = Opm::BlackoilPhases::MaxNumPhases;
        std::vector<bool> active(maxnp, false);

        for (int p = 0; p < pu.MaxNumPhases; ++p) {
            active[ p ] = pu.phase_used[ p ] != 0;
        }

        return active;
    }



    template <class PU>
    std::vector<int>
    active2Canonical(const PU& pu)
    {
        const int maxnp = Opm::BlackoilPhases::MaxNumPhases;
        std::vector<int> act2can(maxnp, -1);

        for (int phase = 0; phase < maxnp; ++phase) {
            if (pu.phase_used[ phase ]) {
                act2can[ pu.phase_pos[ phase ] ] = phase;
            }
        }

        return act2can;
    }


} // Anonymous namespace




    FullyImplicitBlackoilSolver::
    FullyImplicitBlackoilSolver(const UnstructuredGrid&         grid ,
                                const BlackoilPropsAdInterface& fluid,
                                const DerivedGeology&           geo  ,
                                const RockCompressibility*      rock_comp_props,
                                const Wells&                    wells,
                                const LinearSolverInterface&    linsolver)
        : grid_  (grid)
        , fluid_ (fluid)
        , geo_   (geo)
        , rock_comp_props_(rock_comp_props)
        , wells_ (wells)
        , linsolver_ (linsolver)
        , active_(activePhases(fluid.phaseUsage()))
        , canph_ (active2Canonical(fluid.phaseUsage()))
        , cells_ (buildAllCells(grid.number_of_cells))
        , ops_   (grid)
        , wops_  (wells)
        , grav_  (gravityOperator(grid_, ops_, geo_))
        , rq_    (fluid.numPhases())
        , residual_ ( { std::vector<ADB>(fluid.numPhases(), ADB::null()),
                        ADB::null(),
                        ADB::null(),
                        ADB::null() } )
    {
    }





    void
    FullyImplicitBlackoilSolver::
    step(const double   dt,
         BlackoilState& x ,
         WellState&     xw)
    {
        const V pvdt = geo_.poreVolume() / dt;

        {
            const SolutionState state = constantState(x, xw);
            computeAccum(state, 0);
        }

        const double atol  = 1.0e-12;
        const double rtol  = 5.0e-8;
        const int    maxit = 15;

        assemble(pvdt, x, xw);

        const double r0  = residualNorm();
        int          it  = 0;
        std::cout << "\nIteration         Residual\n"
                  << std::setw(9) << it << std::setprecision(9)
                  << std::setw(18) << r0 << std::endl;
        bool resTooLarge = r0 > atol;
        while (resTooLarge && (it < maxit)) {
            const V dx = solveJacobianSystem();

            updateState(dx, x, xw);

            assemble(pvdt, x, xw);

            const double r = residualNorm();

            resTooLarge = (r > atol) && (r > rtol*r0);

            it += 1;
            std::cout << std::setw(9) << it << std::setprecision(9)
                      << std::setw(18) << r << std::endl;
        }

        if (resTooLarge) {
            std::cerr << "Failed to compute converged solution in " << it << " iterations. Ignoring!\n";
            // OPM_THROW(std::runtime_error, "Failed to compute converged solution in " << it << " iterations.");
        }
    }





    FullyImplicitBlackoilSolver::ReservoirResidualQuant::ReservoirResidualQuant()
        : accum(2, ADB::null())
        , mflux(   ADB::null())
        , b    (   ADB::null())
        , head (   ADB::null())
        , mob  (   ADB::null())
    {
    }





    FullyImplicitBlackoilSolver::SolutionState::SolutionState(const int np)
        : pressure  (    ADB::null())
        , saturation(np, ADB::null())
        , rs        (    ADB::null())
        , qs        (    ADB::null())
        , bhp       (    ADB::null())
    {
    }





    FullyImplicitBlackoilSolver::
    WellOps::WellOps(const Wells& wells)
        : w2p(wells.well_connpos[ wells.number_of_wells ],
              wells.number_of_wells)
        , p2w(wells.number_of_wells,
              wells.well_connpos[ wells.number_of_wells ])
    {
        const int        nw   = wells.number_of_wells;
        const int* const wpos = wells.well_connpos;

        typedef Eigen::Triplet<double> Tri;

        std::vector<Tri> scatter, gather;
        scatter.reserve(wpos[nw]);
        gather .reserve(wpos[nw]);

        for (int w = 0, i = 0; w < nw; ++w) {
            for (; i < wpos[ w + 1 ]; ++i) {
                scatter.push_back(Tri(i, w, 1.0));
                gather .push_back(Tri(w, i, 1.0));
            }
        }

        w2p.setFromTriplets(scatter.begin(), scatter.end());
        p2w.setFromTriplets(gather .begin(), gather .end());
    }





    FullyImplicitBlackoilSolver::SolutionState
    FullyImplicitBlackoilSolver::constantState(const BlackoilState& x,
                                               const WellState&     xw)
    {
        const int nc = grid_.number_of_cells;
        const int np = x.numPhases();

        // The block pattern assumes the following primary variables:
        //    pressure
        //    water saturation (if water present)
        //    gas saturation (if gas present)
        //    gas solution factor (if both gas and oil present)
        //    well rates per active phase and well
        //    well bottom-hole pressure
        // Note that oil is assumed to always be present, but is never
        // a primary variable.
        assert(active_[ Oil ]);
        std::vector<int> bpat(np, nc);
        const bool gasandoil = (active_[ Oil ] && active_[ Gas ]);
        if (gasandoil) {
            bpat.push_back(nc);
        }
        bpat.push_back(xw.bhp().size() * np);
        bpat.push_back(xw.bhp().size());

        SolutionState state(np);

        // Pressure.
        assert (not x.pressure().empty());
        const V p = Eigen::Map<const V>(& x.pressure()[0], nc, 1);
        state.pressure = ADB::constant(p, bpat);

        // Saturation.
        assert (not x.saturation().empty());
        const DataBlock s = Eigen::Map<const DataBlock>(& x.saturation()[0], nc, np);
        const Opm::PhaseUsage pu = fluid_.phaseUsage();
        {
            V so = V::Ones(nc, 1);
            if (active_[ Water ]) {
                const int pos = pu.phase_pos[ Water ];
                const V   sw  = s.col(pos);
                so -= sw;

                state.saturation[pos] = ADB::constant(sw, bpat);
            }
            if (active_[ Gas ]) {
                const int pos = pu.phase_pos[ Gas ];
                const V   sg  = s.col(pos);
                so -= sg;

                state.saturation[pos] = ADB::constant(sg, bpat);
            }
            if (active_[ Oil ]) {
                const int pos = pu.phase_pos[ Oil ];
                state.saturation[pos] = ADB::constant(so, bpat);
            }
        }

        // Gas-oil ratio (rs).
        if (active_[ Oil ] && active_[ Gas ]) {
            const V rs = Eigen::Map<const V>(& x.gasoilratio()[0], x.gasoilratio().size());
            state.rs = ADB::constant(rs, bpat);
        } else {
            const V Rs = V::Zero(nc, 1);
            state.rs = ADB::constant(Rs, bpat);
        }

        // Well rates.
        assert (not xw.wellRates().empty());
        // Need to reshuffle well rates, from ordered by wells, then phase,
        // to ordered by phase, then wells.
        const int nw = wells_.number_of_wells;
        // The transpose() below switches the ordering.
        const DataBlock wrates = Eigen::Map<const DataBlock>(& xw.wellRates()[0], nw, np).transpose();
        const V qs = Eigen::Map<const V>(wrates.data(), nw*np);
        state.qs = ADB::constant(qs, bpat);

        // Well bottom-hole pressure.
        assert (not xw.bhp().empty());
        const V bhp = Eigen::Map<const V>(& xw.bhp()[0], xw.bhp().size());
        state.bhp = ADB::constant(bhp, bpat);

        return state;
    }





    FullyImplicitBlackoilSolver::SolutionState
    FullyImplicitBlackoilSolver::variableState(const BlackoilState& x,
                                               const WellState&     xw)
    {
        const int nc = grid_.number_of_cells;
        const int np = x.numPhases();

        std::vector<V> vars0;
        vars0.reserve(active_[Oil] && active_[Gas] ? np + 2 : np + 1); // Rs is primary if oil and gas present.

        // Initial pressure.
        assert (not x.pressure().empty());
        const V p = Eigen::Map<const V>(& x.pressure()[0], nc, 1);
        vars0.push_back(p);

        // Initial saturation.
        assert (not x.saturation().empty());
        const DataBlock s = Eigen::Map<const DataBlock>(& x.saturation()[0], nc, np);
        const Opm::PhaseUsage pu = fluid_.phaseUsage();
        // We do not handle a Water/Gas situation correctly, guard against it.
        assert (active_[ Oil]);
        if (active_[ Water ]) {
            const V sw = s.col(pu.phase_pos[ Water ]);
            vars0.push_back(sw);
        }
        if (active_[ Gas ]) {
            const V sg = s.col(pu.phase_pos[ Gas ]);
            vars0.push_back(sg);
        }

        // Initial gas-oil ratio (Rs).
        if (active_[ Oil ] && active_[ Gas ]) {
            const V rs = Eigen::Map<const V>(& x.gasoilratio()[0], x.gasoilratio().size());
            vars0.push_back(rs);
        }

        // Initial well rates.
        assert (not xw.wellRates().empty());
        // Need to reshuffle well rates, from ordered by wells, then phase,
        // to ordered by phase, then wells.
        const int nw = wells_.number_of_wells;
        // The transpose() below switches the ordering.
        const DataBlock wrates = Eigen::Map<const DataBlock>(& xw.wellRates()[0], nw, np).transpose();
        const V qs = Eigen::Map<const V>(wrates.data(), nw*np);
        vars0.push_back(qs);

        // Initial well bottom-hole pressure.
        assert (not xw.bhp().empty());
        const V bhp = Eigen::Map<const V>(& xw.bhp()[0], xw.bhp().size());
        vars0.push_back(bhp);

        std::vector<ADB> vars = ADB::variables(vars0);

        SolutionState state(np);

        // Pressure.
        int nextvar = 0;
        state.pressure = vars[ nextvar++ ];

        // Saturation.
        const std::vector<int>& bpat = vars[0].blockPattern();
        {
            ADB so  = ADB::constant(V::Ones(nc, 1), bpat);

            if (active_[ Water ]) {
                ADB& sw = vars[ nextvar++ ];
                state.saturation[ pu.phase_pos[ Water ] ] = sw;

                so = so - sw;
            }
            if (active_[ Gas ]) {
                ADB& sg = vars[ nextvar++ ];
                state.saturation[ pu.phase_pos[ Gas ] ] = sg;

                so = so - sg;
            }
            if (active_[ Oil ]) {
                // Note that so is never a primary variable.
                state.saturation[ pu.phase_pos[ Oil ] ] = so;
            }
        }

        // Rs.
        if (active_[ Oil ] && active_[ Gas ]) {
            state.rs = vars[ nextvar++ ];
        } else {
            state.rs = ADB::constant(V::Zero(nc), bpat);
        }

        // Qs.
        state.qs = vars[ nextvar++ ];

        // Bhp.
        state.bhp = vars[ nextvar++ ];

        assert(nextvar == int(vars.size()));

        return state;
    }





    void
    FullyImplicitBlackoilSolver::computeAccum(const SolutionState& state,
                                              const int            aix  )
    {
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();

        const ADB&              press = state.pressure;
        const std::vector<ADB>& sat   = state.saturation;
        const ADB&              rs    = state.rs;

        std::vector<PhasePresence> cond;
        classifyCondition(state, cond);

        const ADB pv_mult = poroMult(press);

        const int maxnp = Opm::BlackoilPhases::MaxNumPhases;
        for (int phase = 0; phase < maxnp; ++phase) {
            if (active_[ phase ]) {
                const int pos = pu.phase_pos[ phase ];
                rq_[pos].b = fluidReciprocFVF(phase, press, rs, cond, cells_);
                rq_[pos].accum[aix] = pv_mult * rq_[pos].b * sat[pos];
                // DUMP(rq_[pos].b);
                // DUMP(rq_[pos].accum[aix]);
            }
        }

        if (active_[ Oil ] && active_[ Gas ]) {
            // Account for gas dissolved in oil.
            const int po = pu.phase_pos[ Oil ];
            const int pg = pu.phase_pos[ Gas ];

            rq_[pg].accum[aix] += state.rs * rq_[po].accum[aix];
            //DUMP(rq_[pg].accum[aix]);
        }
    }





    void
    FullyImplicitBlackoilSolver::
    assemble(const V&             pvdt,
             const BlackoilState& x   ,
             const WellState&     xw  )
    {
        // Create the primary variables.
        const SolutionState state = variableState(x, xw);

        // -------- Mass balance equations --------

        // Compute b_p and the accumulation term b_p*s_p for each phase,
        // except gas. For gas, we compute b_g*s_g + Rs*b_o*s_o.
        // These quantities are stored in rq_[phase].accum[1].
        // The corresponding accumulation terms from the start of
        // the timestep (b^0_p*s^0_p etc.) were already computed
        // in step() and stored in rq_[phase].accum[0].
        computeAccum(state, 1);

        // Set up the common parts of the mass balance equations
        // for each active phase.
        const V transi = subset(geo_.transmissibility(), ops_.internal_faces);
        const std::vector<ADB> kr = computeRelPerm(state);
        const std::vector<ADB> pressures = computePressures(state);
        for (int phaseIdx = 0; phaseIdx < fluid_.numPhases(); ++phaseIdx) {
            computeMassFlux(phaseIdx, transi, kr[phaseIdx], pressures[phaseIdx], state);
            // std::cout << "===== kr[" << phase << "] = \n" << std::endl;
            // std::cout << kr[phase];
            // std::cout << "===== rq_[" << phase << "].mflux = \n" << std::endl;
            // std::cout << rq_[phase].mflux;

            residual_.mass_balance[ phaseIdx ] =
                pvdt*(rq_[phaseIdx].accum[1] - rq_[phaseIdx].accum[0])
                + ops_.div*rq_[phaseIdx].mflux;

            // DUMP(residual_.mass_balance[phase]);
        }

        // -------- Extra (optional) sg or rs equation, and rs contributions to the mass balance equations --------

        // Add the extra (flux) terms to the gas mass balance equations
        // from gas dissolved in the oil phase.
        // The extra terms in the accumulation part of the equation are already handled.
        if (active_[ Oil ] && active_[ Gas ]) {
            const int po = fluid_.phaseUsage().phase_pos[ Oil ];
            const UpwindSelector<double> upwind(grid_, ops_,
                                                rq_[po].head.value());
            const ADB rs_face = upwind.select(state.rs);

            residual_.mass_balance[ Gas ] += ops_.div * (rs_face * rq_[po].mflux);
            // DUMP(residual_.mass_balance[ Gas ]);

            // Also, we have another equation: sg = 0 or rs = rsMax.
            const int pg = fluid_.phaseUsage().phase_pos[ Gas ];
            const ADB sg_eq = state.saturation[pg];
            const ADB rs_max = fluidRsMax(state.pressure, cells_);
            const ADB rs_eq = state.rs - rs_max;
            // Consider the fluid to be saturated if sg >= 1e-14 (a small number)
            Selector<double> use_sat_eq(sg_eq.value()-1e-14);
            residual_.rs_or_sg_eq = use_sat_eq.select(rs_eq, sg_eq);
            // DUMP(residual_.rs_or_sg_eq);
        }

        // -------- Well equation, and well contributions to the mass balance equations --------

        // Contribution to mass balance will have to wait.

        const int nc = grid_.number_of_cells;
        const int np = wells_.number_of_phases;
        const int nw = wells_.number_of_wells;
        const int nperf = wells_.well_connpos[nw];

        const std::vector<int> well_cells(wells_.well_cells, wells_.well_cells + nperf);
        const V transw = Eigen::Map<const V>(wells_.WI, nperf);

        const ADB& bhp = state.bhp;

        const DataBlock well_s = wops_.w2p * Eigen::Map<const DataBlock>(wells_.comp_frac, nw, np).matrix();

        // Extract variables for perforation cell pressures
        // and corresponding perforation well pressures.
        const ADB p_perfcell = subset(state.pressure, well_cells);
        // Finally construct well perforation pressures and well flows.

        // Compute well pressure differentials.
        // Construct pressure difference vector for wells.
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        const int dim = grid_.dimensions;
        const double* g = geo_.gravity();
        if (g) {
            // Guard against gravity in anything but last dimension.
            for (int dd = 0; dd < dim - 1; ++dd) {
                assert(g[dd] == 0.0);
            }
        }

        std::vector<PhasePresence> cond;
        classifyCondition(state, cond);

        ADB cell_rho_total = ADB::constant(V::Zero(nc), state.pressure.blockPattern());
        for (int phase = 0; phase < 3; ++phase) {
            if (active_[phase]) {
                const int pos = pu.phase_pos[phase];
                const ADB cell_rho = fluidDensity(phase, state.pressure, state.rs, cond, cells_);
                cell_rho_total += state.saturation[pos] * cell_rho;
            }
        }
        ADB inj_rho_total = ADB::constant(V::Zero(nperf), state.pressure.blockPattern());
        assert(np == wells_.number_of_phases);
        const DataBlock compi = Eigen::Map<const DataBlock>(wells_.comp_frac, nw, np);
        for (int phase = 0; phase < 3; ++phase) {
            if (active_[phase]) {
                const int pos = pu.phase_pos[phase];
                const ADB cell_rho = fluidDensity(phase, state.pressure, state.rs, cond, cells_);
                const V fraction = compi.col(pos);
                inj_rho_total += (wops_.w2p * fraction.matrix()).array() * subset(cell_rho, well_cells);
            }
        }
        const V rho_perf_cell = subset(cell_rho_total, well_cells).value();
        const V rho_perf_well = inj_rho_total.value();
        V prodperfs = V::Constant(nperf, -1.0);
        for (int w = 0; w < nw; ++w) {
            if (wells_.type[w] == PRODUCER) {
                std::fill(prodperfs.data() + wells_.well_connpos[w],
                          prodperfs.data() + wells_.well_connpos[w+1], 1.0);
            }
        }
        const Selector<double> producer(prodperfs);
        const V rho_perf = producer.select(rho_perf_cell, rho_perf_well);
        const V well_perf_dp = computePerfPress(grid_, wells_, rho_perf, g ? g[dim-1] : 0.0);

        const ADB p_perfwell = wops_.w2p * bhp + well_perf_dp;
        const ADB nkgradp_well = transw * (p_perfcell - p_perfwell);
        // DUMP(nkgradp_well);
        const Selector<double> cell_to_well_selector(nkgradp_well.value());
        ADB well_rates_all = ADB::constant(V::Zero(nw*np), state.bhp.blockPattern());
        ADB perf_total_mob = subset(rq_[0].mob, well_cells);
        for (int phase = 1; phase < np; ++phase) {
            perf_total_mob += subset(rq_[phase].mob, well_cells);
        }
        std::vector<ADB> well_contribs(np, ADB::null());
        std::vector<ADB> well_perf_rates(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            const ADB& cell_b = rq_[phase].b;
            const ADB perf_b = subset(cell_b, well_cells);
            const ADB& cell_mob = rq_[phase].mob;
            const V well_fraction = compi.col(phase);
            // Using total mobilities for all phases for injection.
            const ADB perf_mob_injector = (wops_.w2p * well_fraction.matrix()).array() * perf_total_mob;
            const ADB perf_mob = producer.select(subset(cell_mob, well_cells),
                                                 perf_mob_injector);
            const ADB perf_flux = perf_mob * (nkgradp_well); // No gravity term for perforations.
            well_perf_rates[phase] = (perf_flux*perf_b);
            const ADB well_rates = wops_.p2w * well_perf_rates[phase];
            well_rates_all += superset(well_rates, Span(nw, 1, phase*nw), nw*np);

            // const ADB well_contrib = superset(perf_flux*perf_b, well_cells, nc);
            well_contribs[phase] = superset(perf_flux*perf_b, well_cells, nc);
            // DUMP(well_contribs[phase]);
            residual_.mass_balance[phase] += well_contribs[phase];
        }
        if (active_[Gas] && active_[Oil]) {
            const int oilpos = pu.phase_pos[Oil];
            const int gaspos = pu.phase_pos[Gas];
            const ADB rs_perf = subset(state.rs, well_cells);
            well_rates_all += superset(wops_.p2w * (well_perf_rates[oilpos]*rs_perf), Span(nw, 1, gaspos*nw), nw*np);
            // DUMP(well_contribs[gaspos] + well_contribs[oilpos]*state.rs);
            residual_.mass_balance[gaspos] += well_contribs[oilpos]*state.rs;
        }

        // Set the well flux equation
        residual_.well_flux_eq = state.qs + well_rates_all;
        // DUMP(residual_.well_flux_eq);

        // Handling BHP and SURFACE_RATE wells.
        V bhp_targets(nw);
        V rate_targets(nw);
        M rate_distr(nw, np*nw);
        for (int w = 0; w < nw; ++w) {
            const WellControls* wc = wells_.ctrls[w];
            if (wc->type[wc->current] == BHP) {
                bhp_targets[w] = wc->target[wc->current];
                rate_targets[w] = -1e100;
            } else if (wc->type[wc->current] == SURFACE_RATE) {
                bhp_targets[w] = -1e100;
                rate_targets[w] = wc->target[wc->current];
                for (int phase = 0; phase < np; ++phase) {
                    rate_distr.insert(w, phase*nw + w) = wc->distr[phase];
                }
            } else {
                OPM_THROW(std::runtime_error, "Can only handle BHP and SURFACE_RATE type controls.");
            }
        }
        const ADB bhp_residual = bhp - bhp_targets;
        const ADB rate_residual = rate_distr * state.qs - rate_targets;
        // Choose bhp residual for positive bhp targets.
        Selector<double> bhp_selector(bhp_targets);
        residual_.well_eq = bhp_selector.select(bhp_residual, rate_residual);
        // DUMP(residual_.well_eq);
    }





    V FullyImplicitBlackoilSolver::solveJacobianSystem() const
    {
        const int np = fluid_.numPhases();
        ADB mass_res = residual_.mass_balance[0];
        for (int phase = 1; phase < np; ++phase) {
            mass_res = vertcat(mass_res, residual_.mass_balance[phase]);
        }
        if (active_[Oil] && active_[Gas]) {
            mass_res = vertcat(mass_res, residual_.rs_or_sg_eq);
        }
        const ADB well_res = vertcat(residual_.well_flux_eq, residual_.well_eq);
        const ADB total_residual = collapseJacs(vertcat(mass_res, well_res));
        // DUMP(total_residual);

        const Eigen::SparseMatrix<double, Eigen::RowMajor> matr = total_residual.derivative()[0];

        V dx(V::Zero(total_residual.size()));
        Opm::LinearSolverInterface::LinearSolverReport rep
            = linsolver_.solve(matr.rows(), matr.nonZeros(),
                               matr.outerIndexPtr(), matr.innerIndexPtr(), matr.valuePtr(),
                               total_residual.value().data(), dx.data());
        if (!rep.converged) {
            OPM_THROW(std::runtime_error,
                      "FullyImplicitBlackoilSolver::solveJacobianSystem(): "
                      "Linear solver convergence failure.");
        }
        return dx;
    }





    namespace {
        struct Chop01 {
            double operator()(double x) const { return std::max(std::min(x, 1.0), 0.0); }
        };
    }





    void FullyImplicitBlackoilSolver::updateState(const V& dx,
                                                  BlackoilState& state,
                                                  WellState& well_state) const
    {
        const int np = fluid_.numPhases();
        const int nc = grid_.number_of_cells;
        const int nw = wells_.number_of_wells;
        const V null;
        assert(null.size() == 0);
        const V zero = V::Zero(nc);
        const V one = V::Constant(nc, 1.0);

        // Extract parts of dx corresponding to each part.
        const V dp = subset(dx, Span(nc));
        int varstart = nc;
        const V dsw = active_[Water] ? subset(dx, Span(nc, 1, varstart)) : null;
        varstart += dsw.size();
        const V dsg = active_[Gas] ? subset(dx, Span(nc, 1, varstart)) : null;
        varstart += dsg.size();
        const V drs = (active_[Water] && active_[Gas]) ? subset(dx, Span(nc, 1, varstart)) : null;
        varstart += drs.size();
        const V dqs = subset(dx, Span(np*nw, 1, varstart));
        varstart += dqs.size();
        const V dbhp = subset(dx, Span(nw, 1, varstart));
        varstart += dbhp.size();
        assert(varstart == dx.size());

        // Pressure update.
        const double dpmaxrel = 0.8;
        const V p_old = Eigen::Map<const V>(&state.pressure()[0], nc, 1);
        const V absdpmax = dpmaxrel*p_old.abs();
        const V dp_limited = sign(dp) * dp.abs().min(absdpmax);
        const V p = (p_old - dp_limited).max(zero);
        std::copy(&p[0], &p[0] + nc, state.pressure().begin());

        // Rs update. Moved before the saturation update because it is
        // needed there.
        if (active_[Oil] && active_[Gas]) {
            const double drsmaxrel = 0.8;
            const V rs_old = Eigen::Map<const V>(&state.gasoilratio()[0], nc);
            const V absdrsmax = drsmaxrel*rs_old.abs();
            const V drs_limited = sign(drs) * drs.abs().min(absdrsmax);
            const V rs = rs_old - drs_limited;
            std::copy(&rs[0], &rs[0] + nc, state.gasoilratio().begin());
        }

        // Saturation updates.
        const double dsmax = 0.3;
        const DataBlock s_old = Eigen::Map<const DataBlock>(& state.saturation()[0], nc, np);
        V so = one;
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        if (active_[ Water ]) {
            const int pos = pu.phase_pos[ Water ];
            const V sw_old = s_old.col(pos);
            const V dsw_limited = sign(dsw) * dsw.abs().min(dsmax);
            const V sw = (sw_old - dsw_limited).unaryExpr(Chop01());
            so -= sw;
            for (int c = 0; c < nc; ++c) {
                state.saturation()[c*np + pos] = sw[c];
            }
        }
        if (active_[ Gas ]) {
            const int pos = pu.phase_pos[ Gas ];
            const V sg_old = s_old.col(pos);
            const V dsg_limited = sign(dsg) * dsg.abs().min(dsmax);
            V sg = sg_old - dsg_limited;
            if (active_[ Oil ]) {
                // Appleyard chop process.
                const double epsilon = std::sqrt(std::numeric_limits<double>::epsilon());
                const double above_epsilon = 2.0*epsilon;
                const double rs_adjust = 1.0;
                auto sat2usat = (sg_old > 0.0) && (sg <= 0.0);
                Eigen::Map<V> rs(&state.gasoilratio()[0], nc);
                const V rs_sat = fluidRsMax(p, cells_);
                auto over_saturated = ((sg > 0) || (rs > rs_sat*rs_adjust)) && (sat2usat == false);
                auto usat2sat = (sg_old < epsilon) && over_saturated;
                auto zerosg = (sat2usat && sg_old <= above_epsilon);
                auto epssg = (sat2usat && sg_old > epsilon);
                // With no simple support for Matlab-style statements below,
                // we use an explicit for loop.
                // sg(zerosg) = 0.0;
                // sg(epssg) = epsilon;
                // sg(usat2sat) = above_epsilon;
                // rs(sg > 0) = rs_sat(sg > 0);
                // rs(rs > rs_sat*rs_adjust) = rs_sat(rs > rs_sat*rs_adjust);
                for (int c = 0; c < nc; ++c) {

                    if (zerosg[c]) {
                        sg[c] = 0.0;
                    }
                    if (epssg[c]) {
                        sg[c] = epsilon;
                    }
                    if (usat2sat[c]) {
                        sg[c] = above_epsilon;
                    }
                    if (sg[c] > 0.0) {
                        rs[c] = rs_sat[c];
                    }
                    if (rs[c] > rs_sat[c]*rs_adjust) {
                        rs[c] = rs_sat[c];
                    }
                }
            }
            sg.unaryExpr(Chop01());
            so -= sg;
            for (int c = 0; c < nc; ++c) {
                state.saturation()[c*np + pos] = sg[c];
            }
        }
        if (active_[ Oil ]) {
            const int pos = pu.phase_pos[ Oil ];
            for (int c = 0; c < nc; ++c) {
                state.saturation()[c*np + pos] = so[c];
            }
        }

        // Qs update.
        // Since we need to update the wellrates, that are ordered by wells,
        // from dqs which are ordered by phase, the simplest is to compute
        // dwr, which is the data from dqs but ordered by wells.
        const DataBlock wwr = Eigen::Map<const DataBlock>(dqs.data(), np, nw).transpose();
        const V dwr = Eigen::Map<const V>(wwr.data(), nw*np);
        const V wr_old = Eigen::Map<const V>(&well_state.wellRates()[0], nw*np);
        const V wr = wr_old - dwr;
        std::copy(&wr[0], &wr[0] + wr.size(), well_state.wellRates().begin());

        // Bhp update.
        const V bhp_old = Eigen::Map<const V>(&well_state.bhp()[0], nw, 1);
        const V bhp = bhp_old - dbhp;
        std::copy(&bhp[0], &bhp[0] + bhp.size(), well_state.bhp().begin());

    }





    std::vector<ADB>
    FullyImplicitBlackoilSolver::computeRelPerm(const SolutionState& state) const
    {
        const int               nc   = grid_.number_of_cells;
        const std::vector<int>& bpat = state.pressure.blockPattern();

        const ADB null = ADB::constant(V::Zero(nc, 1), bpat);

        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        const ADB sw = (active_[ Water ]
                        ? state.saturation[ pu.phase_pos[ Water ] ]
                        : null);

        const ADB so = (active_[ Oil ]
                        ? state.saturation[ pu.phase_pos[ Oil ] ]
                        : null);

        const ADB sg = (active_[ Gas ]
                        ? state.saturation[ pu.phase_pos[ Gas ] ]
                        : null);

        return fluid_.relperm(sw, so, sg, cells_);
    }


    std::vector<ADB>
    FullyImplicitBlackoilSolver::computePressures(const SolutionState& state) const
    {
        const int               nc   = grid_.number_of_cells;
        const std::vector<int>& bpat = state.pressure.blockPattern();

        const ADB null = ADB::constant(V::Zero(nc, 1), bpat);

        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        const ADB sw = (active_[ Water ]
                        ? state.saturation[ pu.phase_pos[ Water ] ]
                        : null);

        const ADB so = (active_[ Oil ]
                        ? state.saturation[ pu.phase_pos[ Oil ] ]
                        : null);

        const ADB sg = (active_[ Gas ]
                        ? state.saturation[ pu.phase_pos[ Gas ] ]
                        : null);

        // convert the pressure offsets to the capillary pressures
        std::vector<ADB> pressure = fluid_.capPress(sw, so, sg, cells_);
        for (int phaseIdx = 0; phaseIdx < BlackoilPhases::MaxNumPhases; ++phaseIdx) {
#warning "what's the reference phase??"
            if (phaseIdx == BlackoilPhases::Liquid)
                continue;
            pressure[phaseIdx] = pressure[phaseIdx] - pressure[BlackoilPhases::Liquid];
        }

        // add the total pressure to the capillary pressures
        for (int phaseIdx = 0; phaseIdx < BlackoilPhases::MaxNumPhases; ++phaseIdx) {
            pressure[phaseIdx] += state.pressure;
        }

        return pressure;
    }




    std::vector<ADB>
    FullyImplicitBlackoilSolver::computeRelPermWells(const SolutionState& state,
                                                     const DataBlock& well_s,
                                                     const std::vector<int>& well_cells) const
    {
        const int nw = wells_.number_of_wells;
        const int nperf = wells_.well_connpos[nw];
        const std::vector<int>& bpat = state.pressure.blockPattern();

        const ADB null = ADB::constant(V::Zero(nperf), bpat);

        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        const ADB sw = (active_[ Water ]
                        ? ADB::constant(well_s.col(pu.phase_pos[ Water ]), bpat)
                        : null);

        const ADB so = (active_[ Oil ]
                        ? ADB::constant(well_s.col(pu.phase_pos[ Oil ]), bpat)
                        : null);

        const ADB sg = (active_[ Gas ]
                        ? ADB::constant(well_s.col(pu.phase_pos[ Gas ]), bpat)
                        : null);

        return fluid_.relperm(sw, so, sg, well_cells);
    }





    void
    FullyImplicitBlackoilSolver::computeMassFlux(const int               actph ,
                                                 const V&                transi,
                                                 const ADB&              kr    ,
                                                 const ADB&              phasePressure,
                                                 const SolutionState&    state)
    {
        const int canonicalPhaseIdx = canph_[ actph ];

        std::vector<PhasePresence> cond;
        classifyCondition(state, cond);

        const ADB tr_mult = transMult(state.pressure);
        const ADB mu    = fluidViscosity(canonicalPhaseIdx, phasePressure, state.rs, cond, cells_);

        rq_[ actph ].mob = tr_mult * kr / mu;

        const ADB rho   = fluidDensity(canonicalPhaseIdx, phasePressure, state.rs, cond, cells_);

        ADB& head = rq_[ actph ].head;

        // compute gravity potensial using the face average as in eclipse and MRST
        const ADB rhoavg = ops_.caver * rho;

        const ADB dp = ops_.ngrad * phasePressure - geo_.gravity()[2] * (rhoavg * (ops_.ngrad * geo_.z().matrix()));

        head = transi*dp;
        //head      = transi*(ops_.ngrad * phasePressure) + gflux;

        UpwindSelector<double> upwind(grid_, ops_, head.value());

        const ADB& b       = rq_[ actph ].b;
        const ADB& mob     = rq_[ actph ].mob;
        rq_[ actph ].mflux = upwind.select(b * mob) * head;
        // DUMP(rq_[ actph ].mob);
        // DUMP(rq_[ actph ].mflux);
    }





    double
    FullyImplicitBlackoilSolver::residualNorm() const
    {
        double r = 0;
        for (std::vector<ADB>::const_iterator
                 b = residual_.mass_balance.begin(),
                 e = residual_.mass_balance.end();
             b != e; ++b)
        {
            r = std::max(r, (*b).value().matrix().norm());
        }
        if (active_[Oil] && active_[Gas]) {
            r = std::max(r, residual_.rs_or_sg_eq.value().matrix().norm());
        }
        r = std::max(r, residual_.well_flux_eq.value().matrix().norm());
        r = std::max(r, residual_.well_eq.value().matrix().norm());

        return r;
    }





    ADB
    FullyImplicitBlackoilSolver::fluidViscosity(const int               phase,
                                                const ADB&              p    ,
                                                const ADB&              rs   ,
                                                const std::vector<PhasePresence>& cond,
                                                const std::vector<int>& cells) const
    {
        switch (phase) {
        case Water:
            return fluid_.muWat(p, cells);
        case Oil: {
            return fluid_.muOil(p, rs, cond, cells);
        }
        case Gas:
            return fluid_.muGas(p, cells);
        default:
            OPM_THROW(std::runtime_error, "Unknown phase index " << phase);
        }
    }





    ADB
    FullyImplicitBlackoilSolver::fluidReciprocFVF(const int               phase,
                                                  const ADB&              p    ,
                                                  const ADB&              rs   ,
                                                  const std::vector<PhasePresence>& cond,
                                                  const std::vector<int>& cells) const
    {
        switch (phase) {
        case Water:
            return fluid_.bWat(p, cells);
        case Oil: {
            return fluid_.bOil(p, rs, cond, cells);
        }
        case Gas:
            return fluid_.bGas(p, cells);
        default:
            OPM_THROW(std::runtime_error, "Unknown phase index " << phase);
        }
    }





    ADB
    FullyImplicitBlackoilSolver::fluidDensity(const int               phase,
                                              const ADB&              p    ,
                                              const ADB&              rs   ,
                                              const std::vector<PhasePresence>& cond,
                                              const std::vector<int>& cells) const
    {
        const double* rhos = fluid_.surfaceDensity();
        ADB b = fluidReciprocFVF(phase, p, rs, cond, cells);
        ADB rho = V::Constant(p.size(), 1, rhos[phase]) * b;
        if (phase == Oil && active_[Gas]) {
            // It is correct to index into rhos with canonical phase indices.
            rho += V::Constant(p.size(), 1, rhos[Gas]) * rs * b;
        }
        return rho;
    }





    V
    FullyImplicitBlackoilSolver::fluidRsMax(const V&                p,
                                            const std::vector<int>& cells) const
    {
        return fluid_.rsMax(p, cells);
    }





    ADB
    FullyImplicitBlackoilSolver::fluidRsMax(const ADB&              p,
                                            const std::vector<int>& cells) const
    {
        return fluid_.rsMax(p, cells);
    }





    ADB
    FullyImplicitBlackoilSolver::poroMult(const ADB& p) const
    {
        const int n = p.size();
        if (rock_comp_props_ && rock_comp_props_->isActive()) {
            V pm(n);
            V dpm(n);
            for (int i = 0; i < n; ++i) {
                pm[i] = rock_comp_props_->poroMult(p.value()[i]);
                dpm[i] = rock_comp_props_->poroMultDeriv(p.value()[i]);
            }
            ADB::M dpm_diag = spdiag(dpm);
            const int num_blocks = p.numBlocks();
            std::vector<ADB::M> jacs(num_blocks);
            for (int block = 0; block < num_blocks; ++block) {
                jacs[block] = dpm_diag * p.derivative()[block];
            }
            return ADB::function(pm, jacs);
        } else {
            return ADB::constant(V::Constant(n, 1.0), p.blockPattern());
        }
    }





    ADB
    FullyImplicitBlackoilSolver::transMult(const ADB& p) const
    {
        const int n = p.size();
        if (rock_comp_props_ && rock_comp_props_->isActive()) {
            V tm(n);
            V dtm(n);
            for (int i = 0; i < n; ++i) {
                tm[i] = rock_comp_props_->transMult(p.value()[i]);
                dtm[i] = rock_comp_props_->transMultDeriv(p.value()[i]);
            }
            ADB::M dtm_diag = spdiag(dtm);
            const int num_blocks = p.numBlocks();
            std::vector<ADB::M> jacs(num_blocks);
            for (int block = 0; block < num_blocks; ++block) {
                jacs[block] = dtm_diag * p.derivative()[block];
            }
            return ADB::function(tm, jacs);
        } else {
            return ADB::constant(V::Constant(n, 1.0), p.blockPattern());
        }
    }


    void
    FullyImplicitBlackoilSolver::
    classifyCondition(const SolutionState&        state,
                      std::vector<PhasePresence>& cond ) const
    {
        const PhaseUsage& pu = fluid_.phaseUsage();

        if (active_[ Gas ]) {
            // Oil/Gas or Water/Oil/Gas system
            const int po = pu.phase_pos[ Oil ];
            const int pg = pu.phase_pos[ Gas ];

            const V&  so = state.saturation[ po ].value();
            const V&  sg = state.saturation[ pg ].value();

            cond.resize(sg.size());

            for (V::Index c = 0, e = sg.size(); c != e; ++c) {
                if (so[c] > 0)        { cond[c].setFreeOil  (); }
                if (sg[c] > 0)        { cond[c].setFreeGas  (); }
                if (active_[ Water ]) { cond[c].setFreeWater(); }
            }
        }
        else {
            // Water/Oil system
            assert (active_[ Water ]);

            const int po = pu.phase_pos[ Oil ];
            const V&  so = state.saturation[ po ].value();

            cond.resize(so.size());

            for (V::Index c = 0, e = so.size(); c != e; ++c) {
                cond[c].setFreeWater();

                if (so[c] > 0) { cond[c].setFreeOil(); }
            }
        }
    }


} // namespace Opm
