# Copyright © 2023 Manuel Rodríguez & Zero-Day Labs, Lda.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Author:   Manuel Rodríguez <manuel.cederog@gmail.com>
# Date:     28/03/2023
#
# Description:  Makefile to perform lint checks in the RISC-V IOMMU IP using verilator

WARN_FLAGS :=
WARN_FLAGS += -Wno-LATCH

COMP_FLAGS :=
# COMP_FLAGS += --cc
COMP_FLAGS += --lint-only
# COMP_FLAGS += --report-unoptflat

INC += -I./packages/dependencies
INC += -I./packages/rv_iommu
INC += -I./vendor
INC += -I./include
INC += -I./rtl
INC += -I./rtl/translation_logic
INC += -I./rtl/translation_logic/cdw
INC += -I./rtl/translation_logic/ptw
INC += -I./rtl/translation_logic/wrapper
INC += -I./rtl/software_interface
INC += -I./rtl/software_interface/regmap
INC += -I./rtl/software_interface/wrapper
INC += -I./rtl/ext_interfaces

VERILATOR ?= verilator

all: build sim parse stats plot

lint:
	$(VERILATOR) ${COMP_FLAGS} lint_checks.sv ${INC} ${WARN_FLAGS}

lint2log:
	$(VERILATOR) ${COMP_FLAGS} lint_checks.sv ${INC} ${WARN_FLAGS} 2> verilator.log

build:
	$(VERILATOR) --cc --trace --exe tb_iommu.cpp lint_checks.sv ${INC} ${WARN_FLAGS} -Wno-WIDTH -Wno-UNOPTFLAT --build -o Vlint_checks

sim: build
	./obj_dir/Vlint_checks

parse:
	python3 scripts/parse_vcd.py sim_trace.vcd

stats:
	python3 scripts/calc_stats.py results/translation_trace.json

plot:
	python3 scripts/plot_latency.py results/translation_trace.csv

# ============================================================
# Experiment-specific targets
# ============================================================

# Experiment 1: IOTLB Capacity Thrashing
build1:
	$(VERILATOR) --cc --trace --exe tb1_iommu.cpp lint_checks.sv ${INC} ${WARN_FLAGS} -Wno-WIDTH -Wno-UNOPTFLAT --build -o Vlint_checks

sim1: build1
	./obj_dir/Vlint_checks

exp1: sim1 parse stats plot
	@echo "Experiment 1 (IOTLB Capacity Thrashing) complete."

# Experiment 2: Multi-Device Context Contention
build2:
	$(VERILATOR) --cc --trace --exe tb2_iommu.cpp lint_checks.sv ${INC} ${WARN_FLAGS} -Wno-WIDTH -Wno-UNOPTFLAT --build -o Vlint_checks

sim2: build2
	./obj_dir/Vlint_checks

exp2: sim2 parse stats plot
	@echo "Experiment 2 (Multi-Device Context Contention) complete."

# Experiment 3: Interconnect Memory Contention
build3:
	$(VERILATOR) --cc --trace --exe tb3_iommu.cpp lint_checks.sv ${INC} ${WARN_FLAGS} -Wno-WIDTH -Wno-UNOPTFLAT --build -o Vlint_checks

sim3: build3
	./obj_dir/Vlint_checks

exp3: sim3 parse stats plot
	@echo "Experiment 3 (Interconnect Memory Contention) complete."