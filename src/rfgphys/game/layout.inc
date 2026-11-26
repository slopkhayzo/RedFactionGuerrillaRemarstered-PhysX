// OpenRFG64: rfgphys's struct offsets and sizes (game/layout.h). One line each:
//   RFG_LAYOUT_M(variable, 32-bit value, "type", "member path", byte addend)   a member's offset (+ addend)
//   RFG_LAYOUT_S(variable, 32-bit value, "type")                               a type's size
// The 32-bit values are the upstream constants; the paths were mapped from them with tools/offset_map.py (rfg.pdb ->
// OpenRFG64's PDB). A path OpenRFG64 lacks resolves to 0 and is reported at start-up.

// ---- Havok shapes (physx/havok_shapes.cpp)
RFG_LAYOUT_M(hk_shape_type, 0x08, "hkpShape", "(hkpShapeBase).(hkcdShape).m_type", 0)
RFG_LAYOUT_M(hk_convex_radius, 0x10, "hkpConvexShape", "m_radius", 0)
RFG_LAYOUT_M(hk_triangle_a, 0x20, "hkpTriangleShape", "m_vertexA", 0)
RFG_LAYOUT_M(hk_triangle_b, 0x30, "hkpTriangleShape", "m_vertexB", 0)
RFG_LAYOUT_M(hk_triangle_c, 0x40, "hkpTriangleShape", "m_vertexC", 0)
RFG_LAYOUT_M(hk_box_half_extents, 0x20, "hkpBoxShape", "m_halfExtents", 0)
RFG_LAYOUT_M(hk_convex_vertices_blocks, 0x40, "hkpConvexVerticesShape", "m_rotatedVertices", 0)
RFG_LAYOUT_M(hk_convex_vertices_count, 0x4C, "hkpConvexVerticesShape", "m_numVertices", 0)
RFG_LAYOUT_M(hk_capsule_a, 0x20, "hkpCapsuleShape", "m_vertexA", 0)
RFG_LAYOUT_M(hk_capsule_b, 0x30, "hkpCapsuleShape", "m_vertexB", 0)
RFG_LAYOUT_M(hk_cylinder_radius, 0x14, "hkpCylinderShape", "m_cylRadius", 0)
RFG_LAYOUT_M(hk_cylinder_a, 0x20, "hkpCylinderShape", "m_vertexA", 0)
RFG_LAYOUT_M(hk_cylinder_b, 0x30, "hkpCylinderShape", "m_vertexB", 0)
RFG_LAYOUT_M(hk_convex_translate_child, 0x18, "hkpConvexTranslateShape", "(hkpConvexTransformShapeBase).m_childShape.m_childShape", 0)
RFG_LAYOUT_M(hk_convex_translate_t, 0x20, "hkpConvexTranslateShape", "m_translation", 0)
RFG_LAYOUT_M(hk_convex_transform_child, 0x18, "hkpConvexTransformShape", "(hkpConvexTransformShapeBase).m_childShape.m_childShape", 0)
RFG_LAYOUT_M(hk_convex_transform_t, 0x20, "hkpConvexTransformShape", "m_transform", 0)
RFG_LAYOUT_M(hk_transform_child, 0x14, "hkpTransformShape", "m_childShape.m_childShape", 0)
RFG_LAYOUT_M(hk_transform_t, 0x30, "hkpTransformShape", "m_transform", 0)
RFG_LAYOUT_M(hk_mopp_child, 0x34, "hkpMoppBvTreeShape", "m_child.m_childShape", 0)
RFG_LAYOUT_M(hk_hf_bvtree_child, 0x18, "hkpTriSampledHeightFieldBvTreeShape", "m_childContainer.m_childShape", 0)
RFG_LAYOUT_M(hk_hf_collection_hf, 0x18, "hkpTriSampledHeightFieldCollection", "m_heightfield", 0)
RFG_LAYOUT_M(hk_hf_xres, 0x28, "hkpStorageSampledHeightFieldShape", "(hkpSampledHeightFieldShape).m_xRes", 0)
RFG_LAYOUT_M(hk_hf_zres, 0x2C, "hkpStorageSampledHeightFieldShape", "(hkpSampledHeightFieldShape).m_zRes", 0)
RFG_LAYOUT_M(hk_hf_kind, 0x35, "hkpStorageSampledHeightFieldShape", "(hkpSampledHeightFieldShape).m_heightfieldType", 0)
RFG_LAYOUT_M(hk_hf_int_to_float, 0x40, "hkpStorageSampledHeightFieldShape", "(hkpSampledHeightFieldShape).m_intToFloatScale", 0)
RFG_LAYOUT_M(hk_hf_storage, 0x80, "hkpStorageSampledHeightFieldShape", "m_storage", 0)
RFG_LAYOUT_M(hk_hf_flip, 0x8C, "hkpStorageSampledHeightFieldShape", "m_triangleFlip", 0)
// The compressed height field (offset + scale * uint16) is not in OpenRFG64: 0 there, and that branch never runs.
RFG_LAYOUT_O(hk_hf_compressed_offset, 0x90, "hkpCompressedSampledHeightFieldShape", "m_offset", 0)
RFG_LAYOUT_O(hk_hf_compressed_scale, 0x94, "hkpCompressedSampledHeightFieldShape", "m_scale", 0)

// ---- Havok bodies and the world
RFG_LAYOUT_M(hk_entity_shape, 0x10, "hkpEntity", "(hkpWorldObject).m_collidable", 0)  // m_collidable.m_shape: its first member
RFG_LAYOUT_M(hk_entity_filter_info, 0x2C, "hkpEntity", "(hkpWorldObject).m_collidable.(hkpCollidable).m_broadPhaseHandle.m_collisionFilterInfo", 0)
RFG_LAYOUT_M(hk_entity_uid, 0xD0, "hkpEntity", "m_uid", 0)
RFG_LAYOUT_M(hk_entity_motion, 0xE0, "hkpEntity", "m_motion", 0)
RFG_LAYOUT_M(hk_entity_motion_type, 0xE8, "hkpEntity", "m_motion.(hkpKeyframedRigidMotion).(hkpMotion).m_type", 0)
RFG_LAYOUT_M(hk_entity_motion_transform, 0xF0, "hkpEntity", "m_motion.(hkpKeyframedRigidMotion).(hkpMotion).m_motionState", 0)  // m_transform first
RFG_LAYOUT_M(hk_world_fixed_island, 0x20, "hkpWorld", "m_fixedIsland", 0)
RFG_LAYOUT_M(hk_island_entities, 0x4C, "hkpSimulationIsland", "m_entities", 0)  // hkArray data pointer first
RFG_LAYOUT_M(hk_island_entity_count, 0x50, "hkpSimulationIsland", "m_entities.m_size", 0)

// ---- game Havok glue
RFG_LAYOUT_M(game_filter_layer_table, 0x34, "havok_game_filter", "m_layer_lookup_table", 0)
RFG_LAYOUT_M(destroyable_existing_children, 0x48, "rfg_destroyable_shape", "num_existing_child_shapes", 0)
RFG_LAYOUT_S(raycast_input_size, 0x30, "raycast_input")
RFG_LAYOUT_S(raycast_output_size, 0x0C, "raycast_output")
RFG_LAYOUT_M(raycast_output_count, 0x04, "raycast_output", "m_hits.m_size", 0)
RFG_LAYOUT_S(raycast_hit_size, 0x38, "raycast_output::hit_info")
RFG_LAYOUT_M(raycast_hit_fraction, 0x24, "raycast_output::hit_info", "hit_fraction", 0)
RFG_LAYOUT_M(raycast_hit_object, 0x28, "raycast_output::hit_info", "hit_object_handle", 0)
RFG_LAYOUT_M(raycast_hit_havok_handle, 0x30, "raycast_output::hit_info", "hit_havok_handle", 0)

// ---- objects
RFG_LAYOUT_M(object_pos, 0x04, "object", "pos", 0)
RFG_LAYOUT_M(object_handle, 0x6C, "object", "handle", 0)
RFG_LAYOUT_M(object_type, 0x7E, "object", "obj_type", 0)

// ---- render instances (physx/render_routing.cpp, debris_offload.cpp)
RFG_LAYOUT_M(inst_flags, 0x40, "rl_scene_entity", "m_flags", 0)
RFG_LAYOUT_M(inst_sort_method, 0x100, "rl_renderable_instance", "m_sort_method", 0)

// ---- contacts (core/contacts.cpp): hkpCdBody::m_parent up to the root collidable's filter info
RFG_LAYOUT_M(hk_cdbody_parent, 0x0C, "hkpCdBody", "m_parent", 0)
RFG_LAYOUT_M(hk_collidable_filter_info, 0x1C, "hkpCollidable", "m_broadPhaseHandle.m_collisionFilterInfo", 0)

// ---- objects and movers (physx/debris_offload.cpp, experiments/keep_debris.cpp)
RFG_LAYOUT_M(object_attach, 0x40, "object", "child_constraint_ptr", 0)  // then host_constraint_ptr, attach_info: 3 pointers
RFG_LAYOUT_M(object_havok_handle, 0x4C, "object", "havok_handle", 0)
RFG_LAYOUT_M(object_flags, 0x54, "object", "obj_flags", 0)
RFG_LAYOUT_M(object_stream_flags, 0x55, "object", "obj_flags", 1)  // the flags' second byte (bitfields)
RFG_LAYOUT_M(object_srid, 0x98, "object", "srid", 0)
RFG_LAYOUT_M(object_forward, 0x28, "object", "orient.fvec", 0)
RFG_LAYOUT_M(mover_render_alpha, 0xA0, "object_mover", "m_render_opacity", 0)
RFG_LAYOUT_M(mover_chunk_uid, 0xA4, "object_mover", "chunk_uid", 0)
RFG_LAYOUT_M(mover_flags, 0xE0, "object_mover", "mover_flags", 0)
RFG_LAYOUT_M(gm_render_instance, 0x118, "general_mover", "rdd.m_data.model_instance_p", 0)
RFG_LAYOUT_M(gm_original_object, 0x128, "general_mover", "original_object", 0)
RFG_LAYOUT_M(rfg_mover_destroyable_shape, 0x14C, "rfg_mover", "rdd", 0)  // its first member: the shape
RFG_LAYOUT_M(rfg_mover_has_destroyable, 0x154, "rfg_mover", "rdd.m_loaded", 0)
RFG_LAYOUT_M(human_melee_move, 0x97C, "human", "combat.cur_melee_combat_move", 0)

// ---- world and its zones
RFG_LAYOUT_M(world_all_objects, 0x64, "world", "all_objects.elt", 0)
RFG_LAYOUT_M(world_object_count, 0x6C, "world", "all_objects.num", 0)
RFG_LAYOUT_M(world_general_movers, 0x3C4, "world", "subtype_objects[0].elt", 0)
RFG_LAYOUT_M(world_general_mover_count, 0x3CC, "world", "subtype_objects[0].num", 0)
RFG_LAYOUT_M(world_projectiles, 0x444, "world", "subtype_objects[8].elt", 0)
RFG_LAYOUT_M(world_projectile_count, 0x44C, "world", "subtype_objects[8].num", 0)
RFG_LAYOUT_M(world_zone_count, 0x271C, "world", "num_territory_zones", 0)
RFG_LAYOUT_M(world_zones, 0x2720, "world", "all_zones", 0)
RFG_LAYOUT_M(zone_name, 0x18, "world_zone", "name", 0)
RFG_LAYOUT_M(zone_state, 0x58, "world_zone", "state", 0)
RFG_LAYOUT_M(zone_handle_bits, 0xE0, "world_zone", "obj_handle_mgr.status", 0)
RFG_LAYOUT_M(zone_obj, 0xD4, "world_zone", "zone_objp", 0)
RFG_LAYOUT_M(zone_gid, 0x47C, "world_zone", "gid", 0)

// ---- Havok rigid bodies
RFG_LAYOUT_M(hk_world_object_world, 0x08, "hkpEntity", "(hkpWorldObject).m_world", 0)
RFG_LAYOUT_M(hk_body_properties, 0x7C, "hkpRigidBody", "(hkpEntity).(hkpWorldObject).m_properties", 0)  // hkArray data first
RFG_LAYOUT_M(hk_body_property_count, 0x80, "hkpRigidBody", "(hkpEntity).(hkpWorldObject).m_properties.m_size", 0)
RFG_LAYOUT_M(hk_body_max_lin_vel, 0x19A, "hkpRigidBody", "(hkpEntity).m_motion.m_motionState.m_maxLinearVelocity", 0)
RFG_LAYOUT_M(hk_body_max_ang_vel, 0x19B, "hkpRigidBody", "(hkpEntity).m_motion.m_motionState.m_maxAngularVelocity", 0)
RFG_LAYOUT_M(hk_body_inv_mass, 0x1AC, "hkpRigidBody", "(hkpEntity).m_motion.m_inertiaAndMassInv", 12)  // its w
RFG_LAYOUT_M(hk_body_lin_vel, 0x1B0, "hkpRigidBody", "(hkpEntity).m_motion.m_linearVelocity", 0)
RFG_LAYOUT_M(hk_body_ang_vel, 0x1C0, "hkpRigidBody", "(hkpEntity).m_motion.m_angularVelocity", 0)

// ---- render instances
RFG_LAYOUT_M(inst_rot, 0x08, "rl_scene_entity", "m_world_matrix", 0)  // rvec, uvec, fvec first
RFG_LAYOUT_M(inst_pos, 0x2C, "rl_scene_entity", "m_world_matrix.m_translation", 0)
RFG_LAYOUT_M(inst_partition, 0x9C, "rl_scene_entity", "m_partition_system_p", 0)
RFG_LAYOUT_M(inst_mat_map, 0x118, "rl_destroyable_model_instance", "m_mat_map", 0)
RFG_LAYOUT_M(inst_sub_instances, 0x11C, "rl_destroyable_model_instance", "m_sub_instances", 0)
RFG_LAYOUT_M(inst_num_sub_instances, 0x120, "rl_destroyable_model_instance", "m_num_sub_instances", 0)
RFG_LAYOUT_M(inst_num_renderable_sub_instances, 0x124, "rl_destroyable_model_instance", "m_num_renderable_sub_instances", 0)
RFG_LAYOUT_M(inst_recompile_params, 0x154, "rl_destroyable_model_instance", "m_recompile_parameters_p", 0)
RFG_LAYOUT_M(inst_recompile_data, 0x158, "rl_destroyable_model_instance", "m_recompile_data_p", 0)
RFG_LAYOUT_M(shared_original_index_map, 0x0C, "rl_destroyable_model_instance_shared_base", "m_sub_instance_original_index_map", 0)
RFG_LAYOUT_M(shared_base_sub_instances, 0x14, "rl_destroyable_model_instance_shared_base", "m_base_sub_instances", 0)
RFG_LAYOUT_S(sub_instance_size, 0x20, "rl_sub_instance")

// ---- destruction data (the chunks' own records keep their 32-bit layout: they are loaded from the files)
RFG_LAYOUT_M(destroyable_objects, 0x24, "rfg_destroyable_shape", "objects", 0)
RFG_LAYOUT_M(destroyable_links, 0x28, "rfg_destroyable_shape", "links", 0)
RFG_LAYOUT_M(destroyable_parent, 0x3C, "rfg_destroyable_shape", "parent", 0)
RFG_LAYOUT_M(destroyable_bmin, 0x68, "rfg_destroyable_shape", "local_bmin", 0)
RFG_LAYOUT_M(destroyable_mass, 0x80, "rfg_destroyable_shape", "mass", 0)
RFG_LAYOUT_M(base_destroyable_objects, 0x04, "rfg_base_destroyable_shape", "base_objects", 0)
RFG_LAYOUT_M(base_destroyable_num_objects, 0x0C, "rfg_base_destroyable_shape", "num_objects", 0)
RFG_LAYOUT_M(base_destroyable_links, 0x10, "rfg_base_destroyable_shape", "base_links", 0)
RFG_LAYOUT_M(base_destroyable_num_links, 0x14, "rfg_base_destroyable_shape", "num_links", 0)
RFG_LAYOUT_M(material_brittleness, 0x34, "rfg_physical_material", "brittleness", 0)
RFG_LAYOUT_M(material_link_strength, 0x40, "rfg_physical_material", "link_strength", 0)
RFG_LAYOUT_S(material_size, 0x4C, "rfg_physical_material")

// ---- explosions and melee
RFG_LAYOUT_M(explosion_radius, 0x2C, "explosion_info", "m_radius", 0)
RFG_LAYOUT_M(explosion_secondary_radius, 0x30, "explosion_info", "m_secondary_radius", 0)
RFG_LAYOUT_M(explosion_impulse, 0x5C, "explosion_info", "m_impulse_magnitude", 0)
RFG_LAYOUT_M(explosion_expand_ms, 0x64, "explosion_info", "expanding_explosion_duration", 0)
RFG_LAYOUT_M(inst_shared_data, 0x148, "rl_destroyable_model_instance", "m_shared_data", 0)
RFG_LAYOUT_M(recompile_params_valid, 0x08, "rl_mesh_recompile_parameters", "m_valid", 0)
RFG_LAYOUT_M(melee_cast_radius, 0x14, "human_melee_combat_move_data", "cast_radius", 0)
RFG_LAYOUT_M(melee_impact_impulse, 0x28, "human_melee_combat_move_data", "impact_impulse", 0)
RFG_LAYOUT_M(melee_away_scale, 0x30, "human_melee_combat_move_data", "away_impulse_scale", 0)

// ---- debris_offload.cpp's inline offsets
RFG_LAYOUT_M(object_orient, 0x10, "object", "orient", 0)  // rvec, uvec, fvec
RFG_LAYOUT_M(hk_world_active_islands, 0x28, "hkpWorld", "m_activeSimulationIslands", 0)  // hkArray data first
RFG_LAYOUT_M(hk_world_active_island_count, 0x2C, "hkpWorld", "m_activeSimulationIslands.m_size", 0)
RFG_LAYOUT_M(hk_world_inactive_islands, 0x34, "hkpWorld", "m_inactiveSimulationIslands", 0)
RFG_LAYOUT_M(hk_world_inactive_island_count, 0x38, "hkpWorld", "m_inactiveSimulationIslands.m_size", 0)
RFG_LAYOUT_S(collision_event_size, 0x110, "rfg_collision_event")
RFG_LAYOUT_M(collision_event_key, 0x4C, "rfg_collision_event", "collision_base.base_a.shape_key", 0)
RFG_LAYOUT_M(collision_event_object, 0x50, "rfg_collision_event", "collision_base.base_a.obj_handle", 0)
RFG_LAYOUT_M(collision_event_flags, 0xC8, "rfg_collision_event", "collision_base.flags", 0)
RFG_LAYOUT_M(material_density, 0x30, "rfg_physical_material", "density", 0)
RFG_LAYOUT_M(material_restitution, 0x38, "rfg_physical_material", "restitution", 0)
RFG_LAYOUT_M(material_friction, 0x3C, "rfg_physical_material", "friction", 0)
RFG_LAYOUT_M(material_flags, 0x48, "rfg_physical_material", "flags", 0)
RFG_LAYOUT_M(camera_real_pos, 0x2C, "rfg_camera", "real_pos", 0)
RFG_LAYOUT_M(camera_real_orient, 0x50, "rfg_camera", "real_orient", 0)  // rvec, uvec (+0xC), fvec (+0x18)
RFG_LAYOUT_M(weapon_fire_hit_pos, 0x1C, "weapon_fire_info", "hit_pos", 0)
RFG_LAYOUT_M(weapon_fire_hit_normal, 0x28, "weapon_fire_info", "hit_normal", 0)
RFG_LAYOUT_M(render_instance_pool_count, 0x0C, "pool_list<rl_destroyable_model_instance *>", "num", 0)
RFG_LAYOUT_M(init_params_buffer_allocator, 0x14C, "rl_init_params", "m_pBufferAllocator", 0)
RFG_LAYOUT_M(chunk_name, 0x30, "c_chunk_base", "name", 0)
RFG_LAYOUT_M(material_map_count, 0x0C, "rl_material_map", "m_data.num_materials", 0)
RFG_LAYOUT_M(material_map_is_set, 0x10, "rl_material_map", "m_isSet", 0)
RFG_LAYOUT_M(recompile_index_count, 0x10, "rl_mesh_recompile_data", "geometry.indexCount", 0)
RFG_LAYOUT_M(recompile_vertex_stride, 0x2C, "rl_mesh_recompile_data", "vertex_stride", 0)
RFG_LAYOUT_M(recompile_index_stride, 0x2E, "rl_mesh_recompile_data", "index_stride", 0)
RFG_LAYOUT_M(recompile_num_verts, 0x30, "rl_mesh_recompile_data", "num_verts", 0)
RFG_LAYOUT_M(recompile_num_render_blocks, 0x3C, "rl_mesh_recompile_data", "num_render_blocks", 0)
RFG_LAYOUT_M(recompile_index_copy, 0x44, "rl_mesh_recompile_data", "index_data_p", 0)   // the mesh's own copies
RFG_LAYOUT_M(recompile_vertex_copy, 0x48, "rl_mesh_recompile_data", "vertex_data_p", 0)
