sp             := $(sp).x
dirstack_$(sp) := $(d)
d              := $(dir)


SUBDIRS:= \
	# empty line

DIRS:=$(addprefix $(d)/,$(SUBDIRS))

$(eval $(foreach directory, $(DIRS), $(call directory-module,$(directory)) ))


FILES:= \
	aggressive_dead_code_elim_pass.cpp \
	basic_block.cpp \
	block_merge_pass.cpp \
	build_module.cpp \
	ccp_pass.cpp \
	cfg_cleanup_pass.cpp \
	cfg.cpp \
	common_uniform_elim_pass.cpp \
	compact_ids_pass.cpp \
	composite.cpp \
	constants.cpp \
	dead_branch_elim_pass.cpp \
	dead_insert_elim_pass.cpp \
	dead_variable_elimination.cpp \
	decoration_manager.cpp \
	def_use_manager.cpp \
	dominator_analysis.cpp \
	dominator_tree.cpp \
	eliminate_dead_constant_pass.cpp \
	eliminate_dead_functions_pass.cpp \
	feature_manager.cpp \
	flatten_decoration_pass.cpp \
	fold.cpp \
	fold_spec_constant_op_and_composite_pass.cpp \
	freeze_spec_constant_value_pass.cpp \
	function.cpp \
	if_conversion.cpp \
	inline_exhaustive_pass.cpp \
	inline_opaque_pass.cpp \
	inline_pass.cpp \
	insert_extract_elim.cpp \
	instruction.cpp \
	instruction_list.cpp \
	ir_context.cpp \
	ir_loader.cpp \
	local_access_chain_convert_pass.cpp \
	local_redundancy_elimination.cpp \
	local_single_block_elim_pass.cpp \
	local_single_store_elim_pass.cpp \
	local_ssa_elim_pass.cpp \
	loop_descriptor.cpp \
	mem_pass.cpp \
	merge_return_pass.cpp \
	module.cpp \
	optimizer.cpp \
	pass.cpp \
	pass_manager.cpp \
	private_to_local_pass.cpp \
	propagator.cpp \
	redundancy_elimination.cpp \
	remove_duplicates_pass.cpp \
	replace_invalid_opc.cpp \
	scalar_replacement_pass.cpp \
	set_spec_constant_default_value_pass.cpp \
	strength_reduction_pass.cpp \
	strip_debug_info_pass.cpp \
	type_manager.cpp \
	types.cpp \
	unify_const_pass.cpp \
	value_number_table.cpp \
	workaround1209.cpp \
	# empty line


SRC_$(d):=$(addprefix $(d)/,$(FILES)) $(foreach directory, $(DIRS), $(SRC_$(directory)) )


d  := $(dirstack_$(sp))
sp := $(basename $(sp))
