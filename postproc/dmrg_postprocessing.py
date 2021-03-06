"""@package dmrg_postprocessing
@brief Python module for post-processing data files produced by a DMRG.x application.
See @ref Postprocessing module for more info.

@defgroup   Postprocessing
@brief      Python module for post-processing data files produced by a DMRG.x application.

Usage:
    >>> import sys
    >>> sys.path.append("/path/to/DMRG.x/postproc")
    >>> import dmrg_postprocessing as dmrg

"""

##  @addtogroup Postprocessing
#   @{

import os
import numpy as np
import matplotlib.pyplot as plt
import copy
import dmrg_json_utils as ju

class Data:
    """
    Post-processing of a single DMRG run.
    """

    def __init__(self, base_dir, jobs_dir='', label=None):
        """Initializes the Data object.

        This initializer simply fills in the base directory and prepares the private
        variables which will store the corresponding data. The variables are first
        set to None. Whenever some data is requested, these variables will then be filled.

        Args:
            base_dir:       directory containing the data files

        Kwargs:
            jobs_dir:       path to be prepended to base_dir
            label:          label for plots with this data object

        Example:
            >>> data = dmrg.Data("data_dir",jobs_dir="/tmp/data/",label="Data1")
        """
        self._base_dir = os.path.join(jobs_dir,base_dir)
        self._label = label
        self._run = None
        self._sweepIdx = None
        self._steps = None
        self._timings = None
        self._hamPrealloc = None
        self._entSpectra = None
        self._corr = None
        self._corrLookup = None
        self._color = None

    def PathToFile(self,filename=""):
        """Returns the path to a filename prepended with the base directory

        Args:
            filename:       filename to append to the base directory
        Returns:
            A string combining the path and filename
        """
        return os.path.join(self._base_dir,filename)

    def Label(self):
        """
        Returns the label of the Data object
        """
        return self._label

    ##
    #   @defgroup   DMRGRun DMRGRun
    #   @ingroup    Postprocessing
    #   @brief      Postprocessing the DMRGRun.json data file
    #   @addtogroup DMRGRun
    #   @{

    def _LoadRun(self):
        """
        Loads the DMRGRun.json data file, if not loaded, and returns its contents
        """
        if self._run is None:
            self._run = ju.LoadJSONDict(self.PathToFile('DMRGRun.json'))
        return self._run

    def RunData(self):
        """
        Returns the contents of the DMRGRun.json data file
        """
        return self._LoadRun()

    ##
    #   @}

    ##
    #   @defgroup   DMRGSteps DMRGSteps
    #   @ingroup    Postprocessing
    #   @brief      Postprocessing the DMRGSteps.json data file
    #   @addtogroup DMRGSteps
    #   @{

    def _LoadSteps(self):
        """Loads data from DMRGSteps.json, if not loaded, and extracts the indices
        for required data columns.
        """
        if self._steps is None:
            steps = ju.LoadJSONTable(self.PathToFile('DMRGSteps.json'))
            self._stepsHeaders = steps['headers']
            self._idxEnergy = self._stepsHeaders.index('GSEnergy')
            self._idxNSysEnl = self._stepsHeaders.index('NSites_SysEnl')
            self._idxNEnvEnl = self._stepsHeaders.index('NSites_EnvEnl')
            self._idxLoopidx = self._stepsHeaders.index('LoopIdx')
            self._idxNStatesH = self._stepsHeaders.index('NumStates_H')
            self._steps = steps['table']

    def Steps(self,header=None):
        """Returns a specific column of data of DMRGSteps.json according to the
        header string or returns a deepcopy of the entire data table

        Kwargs:
            header:     string corresponding to the 'header' of DMRGSteps.json
        """
        self._LoadSteps()
        if header is None:
            return copy.deepcopy(self._steps)
        else:
            idx = self._stepsHeaders.index(header)
            return np.array([row[idx] for row in self._steps])

    def StepsHeaders(self):
        """Returns the headers of the table in DMRGSteps.json
        """
        self._LoadSteps()
        return self._stepsHeaders

    def SweepIdx(self,show_all=False):
        """Returns the step indices corresponding to the end of a sweep.

        By default this function looks for the maximum LoopIdx in a range.

        Args:
            show_all: If set to `True`, all step indices will be given.
                If `False`, return only those that correspond to where the
                number of sites in the system is equal to the number of sites in
                the environment.

        Returns:
            List of indices corresponding to `GlobIdx`
        """
        NSites_Sys = self.Steps("NSites_Sys")
        NSites_Env = self.Steps("NSites_Env")

        if self._sweepIdx is None:
            StepIdx = self.Steps("StepIdx")
            LoopIdx = self.Steps("LoopIdx")
            # Look for the maximum position for each loop index
            self._sweepIdx = [max(np.where(LoopIdx==i)[0]) for i in list(set(LoopIdx))]

        if show_all:
            return self._sweepIdx
        else:
            # Include only the positions where the number of sites in the system == environment
            return [ j for j in self._sweepIdx if NSites_Sys[j]==NSites_Env[j]]

    def EnergyPerSite(self):
        """Calculates the energy per site of the superblock Hamiltonian for all steps

        Returns:
            Numpy array containing the energy per site at each step
        """
        self._LoadSteps()
        return np.array([row[self._idxEnergy]/(row[self._idxNSysEnl]+row[self._idxNEnvEnl]) for row in self._steps])

    def NumStatesSuperblock(self,n=None):
        """Determines the number of states in the superblock Hamiltonian.

        If `n` is `None`, the number of sates for all steps will be returned.

        Args:
            n: Index of the step.

        Returns:
            Single integer or array representing the number/s of states
        """
        self._LoadSteps()
        if n is None:
            self._NStatesH = np.array([row[self._idxNStatesH] for row in self._steps])
            return self._NStatesH
        else:
            return self._steps[n][self._idxNStatesH]

    def PlotEnergyPerSite(self,**kwargs):
        """Plots the energy per site of the superblock Hamiltonian as a function of DMRG steps.
        The given label and all kwargs will be passed to the plt.plot function.

        Example:
            >>> data = dmrg.Data("data_dir",jobs_dir="/tmp/data/",label="Data1")
            >>> data.PlotEnergyPerSite(marker='.',color='r')
            >>> plt.show()
        """
        self._LoadSteps()
        energy_iter = np.array(self.EnergyPerSite())
        self._p = plt.plot(energy_iter,label=self._label,**kwargs)
        self._color = self._p[-1].get_color()
        plt.xlabel('DMRG Steps')
        plt.ylabel(r'$E_0/N$')

    def PlotErrorEnergyPerSite(self,which='abs',compare_with='min',**kwargs):
        """Plots the error in the energy per site of the superblock Hamiltonian as a function of DMRG steps.
        Kwargs will be passed to the plt.plot function.

        Example:
            >>> data = dmrg.Data("data_dir",jobs_dir="/tmp/data/",label="Data1")
            >>> data.PlotErrorEnergyPerSite(marker='.',which='rel',color='r')
            >>> plt.show()

        Args:
            which:          Show either the absolute `'abs'` or relative `'rel'` error
            compare_with:   Compare against the minimum `'min'` or `'last'` obtained value
            **kwargs:       keyword arguments to be passed to plt.semilogy

        Raises:
            ValueError:     When options are not matched
        """
        self._LoadSteps()
        if compare_with=='min':
            best = lambda energy_iter: np.min(energy_iter)
        elif compare_with=='last':
            best = lambda energy_iter: energy_iter[-1]
        else:
            raise ValueError("The value of compare_with can only be 'min' or 'last'. Got {}".format(compare_with))
        if which=='abs':
            energy_iter = np.array(self.EnergyPerSite())
            energy_iter = np.abs((energy_iter - best(energy_iter)))
            plt.ylabel(r'$E_0/N - \mathrm{min}(E_0/N)$')
        elif which=='rel':
            energy_iter = np.array(self.EnergyPerSite())
            energy_iter = np.abs((energy_iter - best(energy_iter))/best(energy_iter))
            plt.ylabel(r'$[E_0/N - \mathrm{min}(E_0/N)] / \mathrm{min}(E_0/N)$')
        else:
            raise ValueError("The value of which can only be 'abs' or 'rel'. Got {}".format(which))

        self._p = plt.semilogy(energy_iter,label=self._label,**kwargs)
        self._color = self._p[-1].get_color()
        plt.xlabel('DMRG Steps')

    def PlotLoopBars(self,my_color=None,**kwargs):
        """Plot vertical lines corresponding to the end of each sweep

        Args:
            my_color:   Override default colors
            **kwargs:   keyword arguments to be passed to plt.axvline
        """
        self._LoadSteps()
        LoopIdx = [row[self._idxLoopidx] for row in self._steps]
        dm = np.where([LoopIdx[i] - LoopIdx[i-1] for i in range(1,len(LoopIdx))])[0]
        for d in dm:
            if my_color is not None:
                plt.axvline(x=d,color=my_color,linewidth=1,**kwargs)
            elif self._color is None:
                plt.axvline(x=d,linewidth=1,**kwargs)
            else:
                plt.axvline(x=d,color=self._color,linewidth=1,**kwargs)

    ##
    #   @}

    #
    #   Timings data
    #
    def _LoadTimings(self):
        if self._timings is None:
            timings = ju.LoadJSONTable(self.PathToFile('Timings.json'))
            self._timingsHeaders = timings['headers']
            self._idxTimeTot = self._timingsHeaders.index('Total')
            self._timings = timings['table']

    def Timings(self):
        self._LoadTimings()
        return copy.deepcopy(self._timings)

    def TimingsHeaders(self):
        self._LoadTimings()
        return copy.deepcopy(self._timingsHeaders)

    def TotalTime(self):
        self._LoadTimings()
        return np.array([row[self._idxTimeTot] for row in self._timings])

    def PlotTotalTime(self,which='plot',cumulative=False,units='sec',**kwargs):
        totTime = self.TotalTime()
        ylabel = "Time Elapsed per Step"

        if cumulative:
            totTime = np.cumsum(totTime)
            ylabel = "Cumulative Time Elapsed"

        if units=='min':
            totTime /= 60.
        elif units=='hrs':
            totTime /= 3600.
        elif units=='sec':
            pass
        else:
            raise ValueError("units='{}' unsupported. Choose among ['sec','min','hrs']".format(units))

        if which=='plot':
            self._p = plt.plot(totTime,label=self._label,**kwargs)
        elif which=='semilogy':
            self._p = plt.semilogy(totTime,label=self._label,**kwargs)
        else:
            raise ValueError("which='{}' unsupported. Choose among ['plot','semilogy']".format(which))

        self._color = self._p[-1].get_color()
        plt.xlabel('DMRG Steps')
        plt.ylabel('{} ({})'.format(ylabel,units))

    #
    #   Preallocation data
    #
    def PreallocData(self,n=None,key=None):
        if self._hamPrealloc is None:
            self._hamPrealloc = ju.LoadJSONArray(self.PathToFile('HamiltonianPrealloc.json'))
        if n is None:
            return self._hamPrealloc
        else:
            if key is None:
                return self._hamPrealloc[n]
            else:
                if key=="Tnnz":
                    return np.array(self._hamPrealloc[n]["Dnnz"]) + np.array(self._hamPrealloc[n]["Onnz"])
                else:
                    return self._hamPrealloc[n][key]

    def PlotPreallocData(self,n,totals_only=True,**kwargs):
        Dnnz = np.array(self.PreallocData(n)["Dnnz"])
        Onnz = np.array(self.PreallocData(n)["Onnz"])
        self._p = plt.plot(Dnnz+Onnz,label='Tnnz: {}'.format(n),marker='o',**kwargs)
        if not totals_only:
            color = self._p[-1].get_color()
            self._p = plt.plot(Dnnz,label='Dnnz: {}'.format(n),color=color,marker='s',**kwargs)
            self._p = plt.plot(Onnz,label='Onnz: {}'.format(n),color=color,marker='^',**kwargs)

    #
    #   Entanglement Spectra
    #
    def _LoadSpectra(self):
        if self._entSpectra is None:
            ##
            #   @todo
            #   Change the output of DMRG.x from EntanglementSpectra.json
            #   to RDMEigenvalues.json
            #
            spectra = ju.LoadJSONArray(self.PathToFile('EntanglementSpectra.json'))
            # determine which global indices are the ends of a sweep

            self._entSpectra = spectra

    def RDMEigenvalues(self,idx=None):
        ''' Loads the reduced density matrix eigenvalues at the end of each sweep or for
            a particular sweep

        Args:
            idx:    The index of the sweep requested.
                    If None, the data for all indices are returned.
        Returns:
            An array of values when idx is None, or a single value otherwise.
        '''
        self._LoadSpectra()
        if idx is None:
            vals = [self._entSpectra[row]['Sys'] for row in self.SweepIdx()]
        else:
            row = self.SweepIdx()[idx]
            vals = self._entSpectra[row]['Sys']
        return vals

    def EntanglementEntropy(self):
        ''' Calculates the entanglement entropy using eigenvalues from all sectors '''
        a = self.RDMEigenvalues()
        l = [np.concatenate([a[i][j]['vals'] for j in range(len(a[i]))]) for i in range(len(a))]
        return [-np.sum( [np.log(lii)*lii  for lii in li if lii > 0] ) for li in l]

    def PlotEntanglementEntropy(self):
        ''' Plots the entanglement entropy using eigenvalues from all sectors '''
        ent = np.array(self.EntanglementEntropy())
        plt.plot(ent,'-o')
        plt.xticks(np.arange(ent.shape[0]))
        plt.xlabel("Sweeps")
        plt.ylabel(r"Von Neumann Entropy (S)")

    #
    #   Correlations
    #
    def _LoadCorrelations(self):
        if self._corr is None:
            self._corr = ju.LoadJSONTable(self.PathToFile('Correlations.json'))

    def _LoadCorrelationsLookup(self):
        self._LoadCorrelations()
        if self._corrLookup is None:
            self._corrLookup = {}
            for key in self._corr['info'][0].keys(): self._corrLookup[key] = []
            for corr in self._corr['info']:
                for key in corr:
                    self._corrLookup[key].append(corr[key])

    def Correlations(self):
        self._LoadCorrelations()
        return self._corr

    def CorrelationsInfo(self):
        return self.Correlations()['info']

    def CorrelationsValues(self, key=None, labels=None):
        if labels is None:
            return np.array(self.Correlations()['values'])
        elif labels is not None and key is None:
            raise ValueError('key must be given when labels is not None')
        else:
            idx = self.CorrelationsInfoGetIndex(key, labels)
            return np.array(self.Correlations()['values'])[:,idx]

    def CorrelationsInfoGetIndex(self, key=None, value=None):
        self._LoadCorrelationsLookup()

        if key is None and value is None:
            return self._corrLookup

        elif key is not None and value is None:
            try:
                return self._corrLookup[key]
            except KeyError:
                raise ValueError("Incorrect key='{}'. Choose among {}".format(key, self._corrLookup.keys()))

        elif key is None and value is not None:
            raise ValueError('key must be given when value is not None')

        else: # both key and value are not none
            if isinstance(value,(list,tuple,np.ndarray)):
                return [self._corrLookup[key].index(v) for v in value]
            else:
                return self._corrLookup[key].index(value)

class DataSeries:
    """
    Post-processing for multiple DMRG runs
    """

    def __init__(self, base_dir_list, label_list=None,  *args, **kwargs):
        if base_dir_list:
            # if non-empty check whether the first entry is a tuple
            if len(base_dir_list[0]) == 2:
                dirs = [dl[0] for dl in base_dir_list]
                labels = [dl[1] for dl in base_dir_list]
                base_dir_list = dirs
                label_list = labels
                # note: overrides the input label_list
        if label_list is None:
            self.DataList = [ Data(base_dir, *args, label=base_dir, **kwargs) for base_dir in base_dir_list ]
        else:
            self.DataList = [ Data(base_dir, *args, label=label, **kwargs) for (base_dir, label) in zip(base_dir_list,label_list) ]

    def Data(self,n):
        return self.DataList[n]

    def PlotEnergyPerSite(self,**kwargs):
        for obj in self.DataList: obj.PlotEnergyPerSite(**kwargs)

    def PlotErrorEnergyPerSite(self,which='abs',**kwargs):
        for obj in self.DataList: obj.PlotErrorEnergyPerSite(which='abs',**kwargs)

    def PlotTotalTime(self,**kwargs):
        for obj in self.DataList: obj.PlotTotalTime(**kwargs)

    def PlotLoopBars(self,n=None,**kwargs):
        if n is None:
            for obj in self.DataList: obj.PlotLoopBars(**kwargs)
        else:
            self.DataList[n].PlotLoopBars(**kwargs)

##
#   @}
