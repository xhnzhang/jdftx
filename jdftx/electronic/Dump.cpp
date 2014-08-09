/*-------------------------------------------------------------------
Copyright 2011 Ravishankar Sundararaman, Kendra Letchworth Weaver

This file is part of JDFTx.

JDFTx is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

JDFTx is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with JDFTx.  If not, see <http://www.gnu.org/licenses/>.
-------------------------------------------------------------------*/

#include <electronic/Dump.h>
#include <electronic/Dump_internal.h>
#include <electronic/Everything.h>
#include <electronic/ColumnBundle.h>
#include <electronic/SpeciesInfo.h>
#include <electronic/operators.h>
#include <electronic/ExactExchange.h>
#include <electronic/DOS.h>
#include <electronic/Polarizability.h>
#include <electronic/LatticeMinimizer.h>
#include <fluid/FluidSolver.h>
#include <core/DataMultiplet.h>
#include <core/DataIO.h>
#include <ctime>

void Dump::setup(const Everything& everything)
{	e = &everything;
	if(dos) dos->setup(everything);
	
	//Add citation for QMC coupling if required (done here so that it works in dry run):
	for(auto dumpPair: *this)
		if(dumpPair.second == DumpQMC)
			Citations::add("Quantum Monte Carlo solvation",
				"K.A. Schwarz, R. Sundararaman, K. Letchworth-Weaver, T.A. Arias and R. Hennig, Phys. Rev. B 85, 201102(R) (2012)");
}


void Dump::operator()(DumpFrequency freq, int iter)
{
	if(!checkInterval(freq, iter)) return; // => don't dump this time
	
	bool foundVars = false; //whether any variables are to be dumped at this frequency
	for(auto entry: *this)
		if(entry.first==freq && entry.second!=DumpNone)
		{	foundVars = true;
			break;
		}
	if(!foundVars) return;
	logPrintf("\n");
	
	const ElecInfo &eInfo = e->eInfo;
	const ElecVars &eVars = e->eVars;
	const IonInfo &iInfo = e->iInfo;

	bool isCevec = (freq==DumpFreq_Gummel) || (freq==DumpFreq_Ionic) || (freq==DumpFreq_End); //whether C has been set to eigenfunctions
	
	//Macro to determine which variables should be dumped:
	//(Drop the "Dump" from the DumpVariables enum name to use as var)
	#define ShouldDump(var) (  ShouldDumpNoAll(var) || count(std::make_pair(freq,DumpAll)) )

	//Determine whether to dump, but don't include in the 'All' collection
	#define ShouldDumpNoAll(var) count(std::make_pair(freq,Dump##var))

	#define StartDump(prefix) \
		string fname = getFilename(prefix); \
		logPrintf("Dumping '%s' ... ", fname.c_str()); logFlush();

	#define EndDump \
		logPrintf("done\n"); logFlush();
	

	#define DUMP_nocheck(object, prefix) \
		{	StartDump(prefix) \
			if(mpiUtil->isHead()) saveRawBinary(object, fname.c_str()); \
			EndDump \
		}
	
	#define DUMP_spinCollection(object, prefix) \
		{	if(object.size()==1) DUMP_nocheck(object[0], prefix) \
			else \
			{	DUMP_nocheck(object[0], prefix "_up") \
				DUMP_nocheck(object[1], prefix "_dn") \
				if(object.size()==4) \
				{	DUMP_nocheck(object[2], prefix "_re") \
					DUMP_nocheck(object[3], prefix "_im") \
				} \
			} \
		}

	#define DUMP(object, prefix, varname) \
		if(ShouldDump(varname) && object) \
		{	DUMP_nocheck(object, prefix) \
		}
	
	// Set up date/time stamp
	time_t timenow = time(0);
	tm *mytm = localtime(&timenow);
	ostringstream stampStream;
	stampStream << (mytm->tm_mon+1) << '.' << mytm->tm_mday << '.'
		<< mytm->tm_hour << '-' << mytm->tm_min << '-' << mytm->tm_sec;
	stamp = stampStream.str();
	
	if((ShouldDump(State) and eInfo.fillingsUpdate!=ElecInfo::ConstantFillings) or ShouldDump(Fillings))
	{	//Dump fillings
		double wInv = eInfo.spinType==SpinNone ? 0.5 : 1.0; //normalization factor from external to internal fillings
		for(int q=eInfo.qStart; q<eInfo.qStop; q++) ((ElecVars&)eVars).F[q] *= (1./wInv);
		StartDump("fillings")
		eInfo.write(eVars.F, fname.c_str());
		EndDump
		for(int q=eInfo.qStart; q<eInfo.qStop; q++) ((ElecVars&)eVars).F[q] *= wInv;
	}
	
	if(ShouldDump(State))
	{
		//Dump wave functions
		StartDump("wfns")
		write(eVars.C, fname.c_str(), eInfo);
		EndDump
		
		if(eVars.fluidSolver)
		{	//Dump state of fluid:
			StartDump("fluidState")
			if(mpiUtil->isHead()) eVars.fluidSolver->saveState(fname.c_str());
			EndDump
		}
		
		if(eInfo.fillingsUpdate!=ElecInfo::ConstantFillings and eInfo.fillingsUpdate==ElecInfo::FermiFillingsAux)
		{	//Dump auxilliary hamiltonian
			StartDump("Haux")
			eInfo.write(eVars.B, fname.c_str());
			EndDump
		}
	}

	if(ShouldDump(IonicPositions) || (ShouldDump(State) && e->ionicMinParams.nIterations>0))
	{	StartDump("ionpos")
		FILE* fp = mpiUtil->isHead() ? fopen(fname.c_str(), "w") : nullLog;
		if(!fp) die("Error opening %s for writing.\n", fname.c_str());
		iInfo.printPositions(fp);  //needs to be called from all processes (for magnetic moment computation)
		if(mpiUtil->isHead())fclose(fp);
		EndDump
	}
	if(ShouldDump(Forces))
	{	StartDump("force")
		if(mpiUtil->isHead()) 
		{	FILE* fp = fopen(fname.c_str(), "w");
			if(!fp) die("Error opening %s for writing.\n", fname.c_str());
			iInfo.forces.print(*e, fp);
			fclose(fp);
		}
		EndDump
	}
	if(ShouldDump(Lattice) || (ShouldDump(State) && e->latticeMinParams.nIterations>0))
	{	StartDump("lattice")
		if(mpiUtil->isHead()) 
		{	FILE* fp = fopen(fname.c_str(), "w");
			if(!fp) die("Error opening %s for writing.\n", fname.c_str());
			fprintf(fp, "lattice");
			for(int j=0; j<3; j++)
			{	fprintf(fp, " \\\n\t");
				for(int k=0; k<3; k++)
					fprintf(fp, "%20.15lf ", e->gInfo.R(j,k));
			}
			fprintf(fp, "#Note: latt-scale has been absorbed into these lattice vectors.\n");
			fclose(fp);
		}
		EndDump
	}
	DUMP(I(iInfo.rhoIon), "Nion", IonicDensity)
	
	if(ShouldDump(ElecDensity))
		DUMP_spinCollection(eVars.n, "n")
	if(iInfo.nCore) DUMP(iInfo.nCore, "nCore", CoreDensity)
	
	if((ShouldDump(KEdensity) or (e->exCorr.needsKEdensity() and ShouldDump(ElecDensity))))
	{	const auto& tau = (e->exCorr.needsKEdensity() ? e->eVars.tau : e->eVars.KEdensity());
		 DUMP_spinCollection(tau, "tau")
	}

	DUMP(I(eVars.d_vac), "d_vac", Dvac);
	if(eVars.fluidParams.fluidType != FluidNone)
	{	DUMP(I(eVars.d_fluid), "d_fluid", Dfluid);
		DUMP(I(eVars.d_vac + eVars.d_fluid), "d_tot", Dtot);
		DUMP(I(eVars.V_cavity), "V_cavity", Vcavity);
		DUMP(I(eVars.V_cavity + eVars.d_fluid), "V_fluidTot", VfluidTot);
	}

	DUMP(I(iInfo.Vlocps), "Vlocps", Vlocps)
	if(ShouldDump(Vscloc))
		DUMP_spinCollection(eVars.Vscloc, "Vscloc")
	if(ShouldDump(Vscloc) and e->exCorr.needsKEdensity())
		DUMP_spinCollection(eVars.Vtau, "Vtau")
	
	if(ShouldDump(BandEigs) || (ShouldDump(State) && e->exCorr.orbitalDep && isCevec))
	{	StartDump("eigenvals")
		eInfo.write(eVars.Hsub_eigs, fname.c_str());
		EndDump
	}

	if(eInfo.hasU && (ShouldDump(RhoAtom) || ShouldDump(ElecDensity)))
	{	StartDump("rhoAtom")
		if(mpiUtil->isHead())
		{	FILE* fp = fopen(fname.c_str(), "w");
			for(const matrix& m: eVars.rhoAtom) m.write(fp);
			fclose(fp);
		}
		EndDump
	}
	
	if(eInfo.hasU && ShouldDump(Vscloc))
	{	StartDump("U_rhoAtom")
		if(mpiUtil->isHead())
		{	FILE* fp = fopen(fname.c_str(), "w");
			for(const matrix& m: eVars.U_rhoAtom) m.write(fp);
			fclose(fp);
		}
		EndDump
	}
	
	if(ShouldDump(Ecomponents))
	{	StartDump("Ecomponents")
		if(mpiUtil->isHead())
		{	FILE* fp = fopen(fname.c_str(), "w");
			if(!fp) die("Error opening %s for writing.\n", fname.c_str());	
			e->ener.print(fp);
			fclose(fp);
		}
		EndDump
	}
	
	if((ShouldDump(BoundCharge) || ShouldDump(SolvationRadii)) && eVars.fluidParams.fluidType!=FluidNone)
	{	DataGptr nboundTilde = (-1.0/(4*M_PI*e->gInfo.detR)) * L(eVars.d_fluid);
		if(iInfo.ionWidth) nboundTilde = gaussConvolve(nboundTilde, iInfo.ionWidth);
		nboundTilde->setGzero(-(J(eVars.get_nTot())+iInfo.rhoIon)->getGzero()); //total bound charge will neutralize system
		if(ShouldDump(SolvationRadii))
		{	StartDump("Rsol")
			if(mpiUtil->isHead()) dumpRsol(I(nboundTilde), fname);
			EndDump
		}
		DUMP(I(nboundTilde), "nbound", BoundCharge)
	}
	
	if(ShouldDump(FluidDensity))
		if(eVars.fluidSolver)
			eVars.fluidSolver->dumpDensities(getFilename("fluid%s").c_str());
	
	if(ShouldDump(QMC) && isCevec) dumpQMC();
	
	if(ShouldDump(Dipole))
	{	StartDump("Moments")
		if(mpiUtil->isHead()) Moments::dumpMoment(*e, fname.c_str(), 1, vector3<>(0.,0.,0.));
		EndDump
	}
	
	if(ShouldDump(SIC))
	{	DumpSelfInteractionCorrection selfInteractionCorrection(*e);
		selfInteractionCorrection.dump(getFilename("SIC").c_str());
	}

	if(ShouldDump(Symmetries))
	{	StartDump("sym")
		if(mpiUtil->isHead())
		{	FILE* fp = fopen(fname.c_str(), "w");
			const std::vector< matrix3<int> >& sym = e->symm.getMatrices();
			for(const matrix3<int>& m: sym)
			{	m.print(fp, "%2d ", false);
				fprintf(fp, "\n");
			}
			fclose(fp);
		}
		EndDump
	}
	
	if(ShouldDump(Kpoints))
	{	StartDump("kPts")
		if(mpiUtil->isHead())
		{	FILE* fp = fopen(fname.c_str(), "w");
			eInfo.kpointsPrint(fp, true);
			fclose(fp);
		}
		EndDump
		if(e->symm.mode != SymmetriesNone)
		{	StartDump("kMap")
			if(mpiUtil->isHead())
			{	FILE* fp = fopen(fname.c_str(), "w");
				e->symm.printKmap(fp);
				fclose(fp);
			}
			EndDump
		}
	}
	
	if(ShouldDump(Gvectors))
	{	StartDump("Gvectors")
		if(mpiUtil->isHead())
		{	FILE* fp = fopen(fname.c_str(), "w");
			for(int q=0; q<eInfo.nStates; q++)
			{	//Header:
				fprintf(fp, "# ");
				eInfo.kpointPrint(fp, q, true);
				fprintf(fp, "\n");
				//G-vectors
				const Basis& basis = e->basis[q];
				for(size_t i=0; i<basis.nbasis; i++)
				{	const vector3<int>& iG = basis.iGarr[i];
					fprintf(fp, "%d %d %d\n", iG[0], iG[1], iG[2]);
				}
				fprintf(fp, "\n");
			}
			fclose(fp);
		}
		EndDump
	}
	
	if(ShouldDump(OrbitalDep) && e->exCorr.orbitalDep && isCevec)
		e->exCorr.orbitalDep->dump();
	
	//----------------- The following are not included in 'All' -----------------------

	if(ShouldDumpNoAll(Excitations))
	{	if(e->eInfo.isNoncollinear()) logPrintf("WARNING: Excitations dump not supported with noncollinear spins (Skipping)\n");
		else
		{	StartDump("Excitations")
			dumpExcitations(*e, fname.c_str());
			EndDump
		}
	}
	
	if(ShouldDumpNoAll(Momenta))
	{	StartDump("momenta")
		std::vector<matrix> momenta(eInfo.nStates);
		for(int q=eInfo.qStart; q<eInfo.qStop; q++) //kpoint/spin
		{	momenta[q] = zeroes(eInfo.nBands, eInfo.nBands*3);
			for(int k=0; k<3; k++) //cartesian direction
				momenta[q].set(0,eInfo.nBands, eInfo.nBands*k,eInfo.nBands*(k+1),
					complex(0,e->gInfo.detR) * (eVars.C[q] ^ D(eVars.C[q], k)) );
		}
		eInfo.write(momenta, fname.c_str(), eInfo.nBands, eInfo.nBands*3);
		EndDump
	}
	
	if(ShouldDumpNoAll(RealSpaceWfns))
	{	for(int q=eInfo.qStart; q<eInfo.qStop; q++)
		{	int nSpinor = eVars.C[q].spinorLength();
			for(int b=0; b<eInfo.nBands; b++) for(int s=0; s<nSpinor; s++)
			{	ostringstream prefixStream;
				prefixStream << "wfns_" << q << '_' << (b*nSpinor+s) << ".rs";
				StartDump(prefixStream.str())
				saveRawBinary(I(eVars.C[q].getColumn(b,s)), fname.c_str());
				EndDump
			}
		}
	}

	if(ShouldDumpNoAll(ExcCompare))
	{	StartDump("ExcCompare") logPrintf("\n");
		FILE* fp = fopen(fname.c_str(), "w");
		if(!fp) die("Error opening %s for writing.\n", fname.c_str());
		//Compute exact exchange if required:
		std::map<double,double> unscaledEXX; //exact exchange energies for each range parameter
		if(e->exCorr.exxFactor()) //use the main functional Exx when available
			unscaledEXX[e->exCorr.exxRange()] = e->ener.E["EXX"] / e->exCorr.exxFactor();
		for(auto exc: e->exCorrDiff)
			if(exc->exxFactor() && unscaledEXX.find(exc->exxRange())==unscaledEXX.end())
			{	double omega = exc->exxRange();
				//Exact exchange not yet computed for this range parameter:
				logPrintf("\tComputing exact exchange with range-parameter %lg  ... ", omega);
				unscaledEXX[omega] = (*e->exx)(1., omega, eVars.F, eVars.C);
				logPrintf("EXX = %.16lf (unscaled)\n", unscaledEXX[omega]); logFlush();
			}
		//KE density for meta-GGAs, if required
		DataRptrCollection tau;
		bool needTau = false;
		for(auto exc: e->exCorrDiff)
			needTau |= exc->needsKEdensity();
		if(needTau)
		{	if(e->exCorr.needsKEdensity()) //Used by main functional i.e. already computed in ElecVars
				tau = eVars.tau;
			else //Not used by main functional, need to compute:
				tau = eVars.KEdensity();
		}
		//Print main functional energy:
		fprintf(fp, "%s(%s) = %.16lf\n", relevantFreeEnergyName(*e),
				e->exCorr.getName().c_str(), relevantFreeEnergy(*e));
		//Compute all the functionals in the list:
		for(auto exc: e->exCorrDiff)
		{	double Enew = relevantFreeEnergy(*e) - (e->ener.E["Exc"] + e->ener.E["EXX"] + e->ener.E["Exc_core"])
				+ (*exc)(eVars.get_nXC(), 0, false, &tau)
				+ (iInfo.nCore ? -(*exc)(iInfo.nCore, 0, false, &iInfo.tauCore) : 0)
				+ exc->exxFactor() * unscaledEXX[exc->exxRange()];
			logPrintf("\t%s = %.16lf for '%s'\n", relevantFreeEnergyName(*e), Enew, exc->getName().c_str());
			fprintf(fp, "%s(%s) = %.16lf\n", relevantFreeEnergyName(*e), exc->getName().c_str(), Enew);
		}
		fclose(fp);
		logPrintf("\t"); EndDump
	}
	
	if(ShouldDumpNoAll(FluidDebug))
		if(eVars.fluidSolver)
			eVars.fluidSolver->dumpDebug(getFilename("fluid%s").c_str());

	if(ShouldDumpNoAll(OptVext))
	{	if(eInfo.spinType == SpinZ)
		{	DUMP(eVars.Vexternal[0], "optVextUp", OptVext)
			DUMP(eVars.Vexternal[1], "optVextDn", OptVext)
		}
		else DUMP(eVars.Vexternal[0], "optVext", OptVext)
	}
	
	if(ShouldDumpNoAll(DOS))
	{	dos->dump();
	}
	
	if(ShouldDumpNoAll(Stress))
	{	
		StartDump("stress")
		
		//This part needs to happen on all processes (since it calls ElecVars::energyAndGrad())
		logSuspend();
		LatticeMinimizer lattMin(*((Everything*) e));
		auto stress = lattMin.calculateStress();
		matrix3<> stressTensor;
		for(size_t i=0; i<lattMin.strainBasis.size(); i++)
			stressTensor += stress[i]*lattMin.strainBasis[i];
		lattMin.restore();
		logResume();
		
		if(mpiUtil->isHead())
		{	FILE* fp = fopen(fname.c_str(), "w");
			if(!fp) die("Error opening %s for writing.\n", fname.c_str());
			
			// Dump stress in strain basis units
			fprintf(fp, "%zu strain basis elements\n", lattMin.strainBasis.size());
			for(const matrix3<>& s: lattMin.strainBasis)
			{	s.print(fp, " %lg ");
				fprintf(fp, "\n");
			}
			fprintf(fp, "\n\n");
			
			fprintf(fp, "stress (in strain units, magnitudes along directions above)\n");
			for(size_t j=0; j<lattMin.strainBasis.size(); j++)
			{	fprintf(fp, "%.5e \t ", stress[j]);
			}
			fprintf(fp, "\n\n");
			
			// Dump stress in lattice units
			fprintf(fp, "stress (in strain units)");
			for(int j=0; j<3; j++)
			{	fprintf(fp, " \\\n\t");
				for(int k=0; k<3; k++)
					fprintf(fp, "%20.15lf ", stressTensor(j,k));
			}

			fclose(fp);
		}
		EndDump
	}
	
	if(ShouldDumpNoAll(XCanalysis))
	{	DataRptrCollection tauW = XC_Analysis::tauWeizsacker(*e); DUMP_spinCollection(tauW, "tauW");
		DataRptrCollection spness = XC_Analysis::spness(*e); DUMP_spinCollection(spness, "spness");
		DataRptrCollection sVh = XC_Analysis::sHartree(*e); DUMP_spinCollection(sVh, "sHartree");
	}

	//Polarizability dump deletes wavefunctions to free memory and should therefore happen at the very end
	if(freq==DumpFreq_End && ShouldDumpNoAll(Polarizability))
	{	polarizability->dump(*e);
	}
}

bool Dump::checkInterval(DumpFrequency freq, int iter) const
{	//Check if dumping should occur at this iteration
	auto intervalFreq = interval.find(freq);
	return (intervalFreq==interval.end() //cound not find interval
		|| ((iter+1) % intervalFreq->second == 0)); //or iteration number is divisible by it
}

string Dump::getFilename(string varName) const
{	//Create a map of substitutions:
	std::map<string,string> subMap;
	subMap["$VAR"] = varName;
	subMap["$STAMP"] = stamp;
	//Apply the substitutions:
	string fname = format;
	while(true)
	{	bool found = false;
		for(auto sub: subMap)
		{	size_t pos = fname.find(sub.first);
			if(pos != string::npos)
			{	fname.replace(pos, sub.first.length(), sub.second);
				found = true;
			}
		}
		if(!found) break; //no matches in this pass
	}
	return fname;
}
