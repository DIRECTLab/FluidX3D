#include "setup.hpp"



#include <fstream>
#include <string>

static inline void append_file(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return;                 
    out << text;
}

void main_setup() { // aerodynamics of the word cow; required extensions in defines.hpp: FP16S, EQUILIBRIUM_BOUNDARIES, SUBGRID, FORCE_FIELD, INTERACTIVE_GRAPHICS or GRAPHICS
	// ################################################################## define simulation box size, viscosity and volume force ###################################################################
	const uint3 lbm_N = resolution(float3(1.0f, 2.0f, 1.0f), 18000u);
	const float si_u = 1.0f;
	const float si_length = 2.4f;
	const float si_T = 10.0f;
	const float si_nu = 1.48E-5f, si_rho = 1.225f;

	const float lbm_length = 0.65f * (float)lbm_N.y;
	const float lbm_u = 0.075f;

	units.set_m_kg_s(lbm_length, lbm_u, 1.0f, si_length, si_u, si_rho);
	const float lbm_nu = units.nu(si_nu);
	const ulong lbm_T = units.t(si_T);

	print_info("Re = " + to_string(to_uint(units.si_Re(si_length, si_u, si_nu))));

	LBM lbm(lbm_N, lbm_nu);

	// ###################################################################################### define geometry ######################################################################################
	const float3x3 rotation =
		float3x3(float3(1, 0, 0), radians(90.0f)) *
		float3x3(float3(0, 1, 0), radians(90.0f)) *
		float3x3(float3(0, 0, 1), radians(0.0f));

	Mesh* mesh = read_stl(get_exe_path() + "../stl/AdwaitaSans-Italic__Sigbovik.stl", lbm.size(), lbm.center(), rotation, lbm_length);
	lbm.voxelize_mesh_on_device(mesh);

	// Export mesh to VTK for ParaView (one-time)
	lbm.write_mesh_to_vtk(mesh); // supported in FluidX3D :contentReference[oaicite:2]{index=2}

	const uint Nx = lbm.get_Nx(), Ny = lbm.get_Ny(), Nz = lbm.get_Nz();
	parallel_for(lbm.get_N(), [&](ulong n) {
		uint x = 0u, y = 0u, z = 0u;
		lbm.coordinates(n, x, y, z);

		if (z == 0u) lbm.flags[n] = TYPE_S; // solid floor
		if (lbm.flags[n] != TYPE_S) lbm.u.y[n] = lbm_u; // initialize y-velocity everywhere except in solid cells
		if (x == 0u || x == Nx - 1u || y == 0u || y == Ny - 1u || z == Nz - 1u) lbm.flags[n] = TYPE_E; // inflow/outflow
	});

	// ####################################################################### run simulation, export images and data ##########################################################################
	lbm.graphics.visualization_modes = VIS_FLAG_SURFACE | VIS_Q_CRITERION;

	// --- output controls ---
    ulong vtk_interval = lbm_T / 5u;
    if (vtk_interval < 1u) vtk_interval = 1u;
    
    ulong log_interval = lbm_T / 500u;
    if (log_interval < 1u) log_interval = 1u;

	// CSV header (force time series). Use write_file/append helpers if you prefer.
	write_file(get_exe_path() + "forces.csv",
		"t_lbm,t_si,Fx_lbm,Fy_lbm,Fz_lbm,Fx_siN,Fy_siN,Fz_siN\n"
	);

#if defined(GRAPHICS) && !defined(INTERACTIVE_GRAPHICS)
    float cam = 1.0f * (float)lbm.get_Nx();
    lbm.graphics.set_camera_centered(0.0f, 0.0f, 0.0f, 2.0f);
	//lbm.graphics.set_camera_centered(-40.0f, 20.0f, 78.0f, 1.25f);
	lbm.run(0u, lbm_T);

	while (lbm.get_t() <= lbm_T) {
		if (lbm.graphics.next_frame(lbm_T, 10.0f)) lbm.graphics.write_frame();

		lbm.run(1u, lbm_T);

		const ulong t = lbm.get_t();

		// Periodic VTK dumps of flow fields
		if (t % vtk_interval == 0u) {
			lbm.rho.write_device_to_vtk();   // density field :contentReference[oaicite:3]{index=3}
			lbm.u.write_device_to_vtk();     // velocity field :contentReference[oaicite:4]{index=4}
			lbm.flags.write_device_to_vtk(); // cell flags (solid/bc) :contentReference[oaicite:5]{index=5}

#ifdef FORCE_FIELD
			// Compute and export boundary force field (per solid cell)
			lbm.update_force_field(); // :contentReference[oaicite:6]{index=6}
			lbm.F.write_device_to_vtk();         // :contentReference[oaicite:7]{index=7}
#endif
		}

		// Force integration + CSV logging
		if (t % log_interval == 0u) {
#ifdef FORCE_FIELD
			lbm.update_force_field(); // ensure F is current :contentReference[oaicite:8]{index=8}
			lbm.F.read_from_device();            // :contentReference[oaicite:9]{index=9}

			double Fx = 0.0, Fy = 0.0, Fz = 0.0;
			for (ulong n = 0u; n < lbm.get_N(); n++) {
				if (lbm.flags[n] == TYPE_S) {
					Fx += (double)lbm.F.x[n];
					Fy += (double)lbm.F.y[n];
					Fz += (double)lbm.F.z[n];
				}
			}

			const double t_si = (double)units.si_t((float)t);
			const double Fx_si = (double)units.si_F((float)Fx);
			const double Fy_si = (double)units.si_F((float)Fy);
			const double Fz_si = (double)units.si_F((float)Fz);

			append_file(get_exe_path() + "forces.csv",
				to_string(t) + "," + to_string(t_si) + "," +
				to_string(Fx) + "," + to_string(Fy) + "," + to_string(Fz) + "," +
				to_string(Fx_si) + "," + to_string(Fy_si) + "," + to_string(Fz_si) + "\n"
			);
#endif
		}
	}

#else
	lbm.run(); // interactive mode; you can add similar periodic dump logic via callbacks if desired
#endif
}
