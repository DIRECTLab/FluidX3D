#include "setup.hpp"


extern std::vector<std::string> main_arguments;

static inline void append_file(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return;                 
    out << text;
}



// ################################################################## parse positional arguments ###################################################################
static inline void usage() {
    std::cout << "FluidX3D (Modified for analysis of fonts to submit to SIGBOVIK 2026)" << std::endl;
    std::cout << "Usage: $ ./make.sh [input .stl file (relative path)] [output folder (absolute path)]" << std::endl;
    exit(0);
}

static std::string input_file;
static std::string output_folder;
static inline void parse_out_arguments() {
    if (main_arguments.size() != 2) usage();
    input_file = main_arguments[0];
    output_folder = main_arguments[1];

    // Ensure output_folder ends with '/'
    if (!output_folder.empty() && output_folder.back() != '/' && output_folder.back() != '\\') {
        output_folder.push_back('/');
    }
}


static inline ulong idx3(const uint x, const uint y, const uint z,
                        const uint Nx, const uint Ny) {
    return (ulong)x + (ulong)Nx * ((ulong)y + (ulong)Ny * (ulong)z);
}

// Projected frontal area onto XZ plane by "first solid hit" per (x,z) ray marched from upstream (y=0) to downstream (y=Ny-1).
// This avoids counting internal cavities / self-shadowing.
static double compute_frontal_area_SI_first_hit(LBM& lbm, const double dx_si) {
    const uint Nx = lbm.get_Nx(), Ny = lbm.get_Ny(), Nz = lbm.get_Nz();
    const double dA = dx_si * dx_si;
    double A = 0.0;

    for (uint z = 0; z < Nz; ++z) {
        for (uint x = 0; x < Nx; ++x) {
            for (uint y = 0; y < Ny; ++y) {
                const ulong n = idx3(x, y, z, Nx, Ny);
                if (lbm.flags[n] == TYPE_S) { // first solid encountered along +Y
                    A += dA;
                    break;
                }
            }
        }
    }
    return A;
}

// Estimate freestream speed U_inf (m/s) from a small upstream slab.
// Reads u to host each call, so call it only at log cadence.
static double estimate_Uinf_si(LBM& lbm) {
    // Make sure flags are on host (solid mask), then read u to host.
    // If flags are already on host, this is cheap.
    lbm.u.read_from_device();

    const uint Nx = lbm.get_Nx(), Ny = lbm.get_Ny(), Nz = lbm.get_Nz();
    const uint y0 = (uint)(0.10 * (double)Ny);
    const uint y1 = std::max(y0 + 1u, (uint)(0.15 * (double)Ny));
    const uint x0 = (uint)(0.10 * (double)Nx);
    const uint x1 = std::max(x0 + 1u, (uint)(0.90 * (double)Nx));
    const uint z0 = (uint)(0.10 * (double)Nz);
    const uint z1 = std::max(z0 + 1u, (uint)(0.90 * (double)Nz));

    double sum_uy_lbm = 0.0;
    ulong count = 0;
    for (uint z = z0; z < z1; ++z) {
        for (uint y = y0; y < y1; ++y) {
            for (uint x = x0; x < x1; ++x) {
                const ulong n = idx3(x, y, z, Nx, Ny);
                if (lbm.flags[n] == TYPE_S) continue;
                sum_uy_lbm += (double)lbm.u.y[n]; // flow assumed +Y
                count++;
            }
        }
    }
    const double uy_lbm = (count > 0) ? (sum_uy_lbm / (double)count) : 0.0;
    return (double)units.si_u((float)uy_lbm);
}
 

void main_setup() { // aerodynamics of a passed in stl file; required extensions in defines.hpp: FP16S, SUBGRID, FORCE_FIELD, INTERACTIVE_GRAPHICS or GRAPHICS
    parse_out_arguments();
	// ################################################################## define simulation box size, viscosity and volume force ###################################################################
	const uint3 lbm_N = resolution(float3(1.0f, 2.0f, 1.0f), 10000u);
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

	Mesh* mesh = read_stl(input_file, lbm.size(), lbm.center(), rotation, lbm_length);
	lbm.voxelize_mesh_on_device(mesh);

	// Export mesh to VTK for ParaView (one-time)
	lbm.write_mesh_to_vtk(mesh, output_folder); // supported in FluidX3D :contentReference[oaicite:2]{index=2}

	const uint Nx = lbm.get_Nx(), Ny = lbm.get_Ny(), Nz = lbm.get_Nz();
	parallel_for(lbm.get_N(), [&](ulong n) {
		uint x = 0u, y = 0u, z = 0u;
		lbm.coordinates(n, x, y, z);

		//if (z == 0u) lbm.flags[n] = TYPE_S; // solid floor
		if (lbm.flags[n] != TYPE_S) lbm.u.y[n] = lbm_u; // initialize y-velocity everywhere except in solid cells
		if (x == 0u || x == Nx - 1u || y == 0u || y == Ny - 1u || z == Nz - 1u) lbm.flags[n] = TYPE_E; // inflow/outflow
	});

    // --- reference quantities for coefficients ---
    const double rho_si = (double)si_rho;    // 1.225 kg/m^3 from your config
    const double dx_si  = (double)units.si_x(1.0f); // meters per lattice cell

    // IMPORTANT: flags were modified on device; pull them to host before computing A_ref on CPU.
    lbm.flags.read_from_device();

    // Compute frontal projected area A_ref (m^2) using first-hit ray marching (better than neighbor-face counting).
    const double A_ref = compute_frontal_area_SI_first_hit(lbm, dx_si);

 

    write_file(output_folder + "ref.txt",
        "dx_si_m=" + to_string(dx_si, 9u) + "\n" +
        "rho_si=" + to_string(rho_si, 6u) + "\n" +
        "A_ref_m2=" + to_string(A_ref, 9u) + "\n" +
        "NOTE=Uinf and q are computed per forces.csv row.\n"
    );

	// ####################################################################### run simulation, export images and data ##########################################################################
	lbm.graphics.visualization_modes = VIS_FLAG_SURFACE | VIS_Q_CRITERION;

	// --- output controls ---
    ulong vtk_interval = lbm_T / 5u;
    if (vtk_interval < 1u) vtk_interval = 1u;
    
    ulong log_interval = lbm_T / 500u;
    if (log_interval < 1u) log_interval = 1u;

	// CSV header (force time series). Use write_file/append helpers if you prefer.
    write_file(output_folder + "forces.csv",
        "t_lbm,t_si,Fx_lbm,Fy_lbm,Fz_lbm,Fx_siN,Fy_siN,Fz_siN,A_ref_m2,Uinf_mps,q_Pa,CD,CL\n"
    );

#if defined(GRAPHICS) && !defined(INTERACTIVE_GRAPHICS)
    float cam = 1.0f * (float)lbm.get_Nx();
    // camera (used for exporting snapshots) looking at mesh from x-axis
    lbm.graphics.set_camera_centered(0.0f, 0.0f, 0.0f, 2.0f);
	lbm.run(0u, lbm_T);

	while (lbm.get_t() <= lbm_T) {
		if (lbm.graphics.next_frame(lbm_T, 10.0f)) lbm.graphics.write_frame_png(output_folder, false);

		lbm.run(1u, lbm_T);

		const ulong t = lbm.get_t();

		// Periodic VTK dumps of flow fields
		if (t % vtk_interval == 0u) {
			lbm.rho.write_device_to_vtk(output_folder);   // density field :contentReference[oaicite:3]{index=3}
			lbm.u.write_device_to_vtk(output_folder);     // velocity field :contentReference[oaicite:4]{index=4}
			lbm.flags.write_device_to_vtk(output_folder); // cell flags (solid/bc) :contentReference[oaicite:5]{index=5}

#ifdef FORCE_FIELD
			// Compute and export boundary force field (per solid cell)
			lbm.update_force_field();                         // :contentReference[oaicite:6]{index=6}
			lbm.F.write_device_to_vtk(output_folder);         // :contentReference[oaicite:7]{index=7}
#endif
		}

		// Force integration + CSV logging
		if (t % log_interval == 0u) {
#ifdef FORCE_FIELD
			lbm.update_force_field();            // ensure F is current :contentReference[oaicite:8]{index=8}
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

            // Estimate actual freestream speed and compute dynamic pressure
            const double Uinf_si = estimate_Uinf_si(lbm);     // m/s
            const double q = 0.5 * rho_si * Uinf_si * Uinf_si; // Pa

            // Coefficients. If you find sign is flipped, change Fy_si -> -Fy_si once and keep it consistent.
            const double CD = (q > 0.0 && A_ref > 0.0) ? (Fy_si / (q * A_ref)) : 0.0; // drag along +Y (assumed)
            const double CL = (q > 0.0 && A_ref > 0.0) ? (Fz_si / (q * A_ref)) : 0.0; // lift along +Z (assumed)
 

            append_file(output_folder + "forces.csv",
                to_string(t) + "," + to_string(t_si) + "," +
                to_string(Fx) + "," + to_string(Fy) + "," + to_string(Fz) + "," +
                to_string(Fx_si) + "," + to_string(Fy_si) + "," + to_string(Fz_si) + "," +
                to_string(A_ref, 9u) + "," + to_string(Uinf_si, 9u) + "," + to_string(q, 9u) + "," +
                to_string(CD, 9u) + "," + to_string(CL, 9u) + "\n"
            );
#endif
		}
	}

#else
	lbm.run(); // interactive mode; you can add similar periodic dump logic via callbacks if desired
#endif
}
