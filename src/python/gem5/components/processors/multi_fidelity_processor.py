from typing import (
    List,
)

import m5
from m5.objects import Root

from ...utils.override import *
from ..boards.abstract_board import AbstractBoard
from .abstract_core import AbstractCore
from .abstract_processor import AbstractProcessor
from .base_cpu_core import BaseCPUCore


class MultiFidelityProcessor(AbstractProcessor):
    def __init__(self, **kwargs) -> None:
        num_cores = -1
        isa = None
        for name, cores in kwargs.items():
            if not isinstance(cores, list) or not isinstance(
                cores[0], BaseCPUCore
            ):
                raise ValueError(
                    "Arguments should be passed as a list of `BaseCPUCore`."
                )
            if len({core.get_isa() for core in cores}) != 1:
                raise ValueError(
                    "All the passed core lists should have the same isa."
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
        self._core_options = list()
        self._current_cores = list()
        self._all_the_cores = list()

        self._board = None

        for index, (name, cores) in enumerate(kwargs.items()):
            self._all_the_cores.extend(cores)

            self._current_cores = (
                cores if not self._current_cores else self._current_cores
            )
            for core in cores:
                core.set_switched_out(index != 0)

            setattr(self, name, cores)
            self._core_options.append(name)

        self._prepare_kvm = any(
            core.is_kvm_core() for core in self._all_cores()
        )
        if self._prepare_kvm:
            from m5.objects import KvmVM

            self.kvm_vm = KvmVM()

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

    def switch(self):
        raise NotImplementedError

    def total_switches(self):
        raise NotImplementedError

    def switch_to_processor(self, switchable_core_key: str):
        # Run various checks.
        if not hasattr(self, "_board"):
            raise AssertionError("The processor has not been incorporated.")

        if switchable_core_key not in self._core_options:
            raise AssertionError(
                f"Key {switchable_core_key} is not a key in the"
                " switchable_processor dictionary."
            )

        # Select the correct processor to switch to.
        to_switch = getattr(self, switchable_core_key)

        # Run more checks.
        if to_switch == self._current_cores:
            raise AssertionError(
                "Cannot swap current cores with the current cores"
            )

        if len(to_switch) != len(self._current_cores):
            raise AssertionError(
                "The number of cores to swap in is not the same as the number "
                "already swapped in. This is not allowed."
            )

        current_core_simobj = [
            core.get_simobject() for core in self._current_cores
        ]
        to_switch_simobj = [core.get_simobject() for core in to_switch]

        # Switch the CPUs
        m5.switchCpus(
            self._board,
            list(zip(current_core_simobj, to_switch_simobj)),
            has_ruby_cache=self._board.get_cache_hierarchy().is_ruby(),
        )

        # Ensure the current processor is updated.
        self._current_cores = to_switch

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
