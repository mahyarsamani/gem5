from itertools import cycle
from typing import Dict, List

import m5
from m5.objects import Root

from ...utils.override import *
from ..boards.abstract_board import AbstractBoard
from .abstract_core import AbstractCore
from .abstract_processor import AbstractProcessor
from .base_cpu_core import BaseCPUCore


class MultiFidelityProcessor(AbstractProcessor):
    def __init__(
        self, start_with: str, **core_map: Dict[str, List[AbstractCore]]
    ) -> None:
        num_cores = -1
        isa = None
        assert (
            start_with in core_map.keys()
        ), "`start_with` should be the name of one of the cores."
        for name, cores in core_map.items():
            if not isinstance(cores, list) or not all(
                isinstance(core, BaseCPUCore) for core in cores
            ):
                raise ValueError(
                    "Arguments should be passed as a list of `BaseCPUCore`."
                )
            if len({core.get_isa() for core in cores}) != 1:
                raise ValueError(
                    "All the passed core lists should have the same isa."
                )
            if (
                len({core.get_simobject().memory_mode() for core in cores})
                != 1
            ):
                raise ValueError(
                    "All the cores in the same list should have the same memory mode."
                )
            num_cores = len(cores) if num_cores == -1 else num_cores
            if num_cores != len(cores):
                raise ValueError(
                    "All the passed core lists should have the same length."
                )
            isa = cores[0].get_isa() if isa is None else isa
            if isa != cores[0].get_isa():
                raise ValueError(
                    "All the passed core lists should have the same isa."
                )

        super().__init__(isa=isa)
        self._core_options = [name for name, _ in core_map.items()]
        self._switched_off_cores = sum(
            [cores for name, cores in core_map.items() if name != start_with],
            [],
        )
        self._current_name = start_with
        self._current_cores = core_map[start_with]
        for core in self._switched_off_cores:
            core.set_switched_out(True)
        for core in self._current_cores:
            core.set_switched_out(False)
        self._all_the_cores = self._current_cores + self._switched_off_cores
        for name, cores in core_map.items():
            setattr(self, name, cores)

        self._board = None
        self._prepare_kvm = any(
            core.is_kvm_core() for core in self._all_cores()
        )
        if self._prepare_kvm:
            from m5.objects import KvmVM

            self.kvm_vm = KvmVM()

        self._fix_cpuid()

    def _fix_cpuid(self):
        for index, core in enumerate(self._current_cores):
            core.get_simobject().cpu_id = index
        for core, prim_core in zip(
            self._switched_off_cores, cycle(self._current_cores)
        ):
            core.get_simobject().cpu_id = prim_core.get_simobject().cpu_id

    def _get_mem_mode(self, cores: List):
        return cores[0].get_simobject().memory_mode()

    @overrides(AbstractProcessor)
    def incorporate_processor(self, board: AbstractBoard) -> None:
        # This is a bit of a hack. The `m5.switchCpus` function, used in the
        # "switch_to_processor" function, requires the System simobject as an
        # argument. We therefore need to store the board when incorporating the
        # procsesor
        self._board = board

        if self._prepare_kvm:
            # To get the KVM CPUs to run on different host CPUs
            # Specify a different event queue for each CPU
            kvm_cores = [
                core for core in self._all_cores() if core.is_kvm_core()
            ]
            for i, core in enumerate(kvm_cores):
                for obj in core.get_simobject().descendants():
                    obj.eventq_index = 0
                core.get_simobject().eventq_index = i + 1

        mem_mode = self._get_mem_mode(self._current_cores)
        if (
            self._board.get_cache_hierarchy().is_ruby()
            and mem_mode == "atomic"
        ):
            mem_mode = "atomic_noncaching"
        self._board.set_mem_mode(mem_mode)

    @overrides(AbstractProcessor)
    def get_num_cores(self) -> int:
        # Note: This is a special case where the total number of cores in the
        # design is not the number of cores, due to some being switched out.
        return len(self._current_cores)

    @overrides(AbstractProcessor)
    def get_cores(self) -> List[AbstractCore]:
        return self._current_cores

    def _all_cores(self):
        yield from self._all_the_cores

    def switch_to_processor(self, core_name: str):
        # Run various checks.
        if self._board is None:
            raise AssertionError("The processor has not been incorporated.")

        if core_name not in self._core_options:
            raise AssertionError(
                f"Key {core_name} is not a key in the"
                " switchable_processor dictionary."
            )

        # Run more checks.
        if core_name == self._current_name:
            raise AssertionError(
                "Cannot swap current cores with the current cores"
            )

        # Select the correct processor to switch to.
        switch_in = getattr(self, core_name)

        if len(switch_in) != len(self._current_cores):
            raise AssertionError(
                "The number of cores to swap in is not the same as the number "
                "already swapped in. This is not allowed."
            )

        switch_out_simobj = [
            core.get_simobject() for core in self._current_cores
        ]
        switch_in_simobj = [core.get_simobject() for core in switch_in]

        # Switch the CPUs
        m5.switchCpus(
            self._board,
            list(zip(switch_out_simobj, switch_in_simobj)),
            has_ruby_cache=self._board.get_cache_hierarchy().is_ruby(),
        )

        # Ensure the current processor is updated.
        self._current_name = core_name
        self._current_cores = switch_in

    def has_phase(self, phase: str) -> bool:
        return phase in self._core_options

    def _pre_instantiate(self, root: Root) -> None:
        super()._pre_instantiate(root)
        # The following is a bit of a hack. If a simulation is to use a KVM
        # core then the `sim_quantum` value must be set. However, in the
        # case of using a SwitchableProcessor the KVM cores may be
        # switched out and therefore not accessible via `get_cores()`.
        # This is the reason for the `isinstance` check.
        #
        # We cannot set the `sim_quantum` value in every simulation as
        # setting it causes the scheduling of exits to be off by the
        # `sim_quantum` value (something necessary if we are using KVM
        # cores). Ergo we only set the value of KVM cores are present.
        #
        # There is still a bug here in that if the user is switching to and
        # from KVM and non-KVM cores via the SwitchableProcessor then the
        # scheduling of exits for the non-KVM cores will be incorrect. This
        # will be fixed at a later date.
        if self._prepare_kvm:
            m5.ticks.fixGlobalFrequency()
            root.sim_quantum = m5.ticks.fromSeconds(0.001)
