/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#endif

#include "GEO_transform.hh"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "DNA_grease_pencil_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_nodes_gizmos_transforms.hh"
#include "BKE_geometry_set.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_instances.hh"
#include "BKE_mesh.hh"
#include "BKE_pointcloud.hh"
#include "BKE_volume.hh"

namespace blender::geometry {

static void translate_positions(MutableSpan<float3> positions, const float3 &translation)
{
  threading::parallel_for(positions.index_range(), 2048, [&](const IndexRange range) {
    for (float3 &position : positions.slice(range)) {
      position += translation;
    }
  });
}

static void transform_positions(MutableSpan<float3> positions, const float4x4 &matrix)
{
  threading::parallel_for(positions.index_range(), 1024, [&](const IndexRange range) {
    for (float3 &position : positions.slice(range)) {
      position = math::transform_point(matrix, position);
    }
  });
}

static void translate_pointcloud(PointCloud &pointcloud, const float3 translation)
{
  if (math::is_zero(translation)) {
    return;
  }

  std::optional<Bounds<float3>> bounds;
  if (pointcloud.runtime->bounds_cache.is_cached()) {
    bounds = pointcloud.runtime->bounds_cache.data();
  }

  bke::MutableAttributeAccessor attributes = pointcloud.attributes_for_write();
  bke::SpanAttributeWriter position = attributes.lookup_or_add_for_write_span<float3>(
      "position", bke::AttrDomain::Point);
  translate_positions(position.span, translation);
  position.finish();

  if (bounds) {
    bounds->min += translation;
    bounds->max += translation;
    pointcloud.runtime->bounds_cache.ensure([&](Bounds<float3> &r_data) { r_data = *bounds; });
  }
}

static void transform_pointcloud(PointCloud &pointcloud, const float4x4 &transform)
{
  bke::MutableAttributeAccessor attributes = pointcloud.attributes_for_write();
  bke::SpanAttributeWriter position = attributes.lookup_or_add_for_write_span<float3>(
      "position", bke::AttrDomain::Point);
  transform_positions(position.span, transform);
  position.finish();
}

static void translate_greasepencil(GreasePencil &grease_pencil, const float3 translation)
{
  using namespace blender::bke::greasepencil;
  for (const int layer_index : grease_pencil.layers().index_range()) {
    Layer &layer = grease_pencil.layer(layer_index);
    float4x4 local_transform = layer.local_transform();
    local_transform.location() += translation;
    layer.set_local_transform(local_transform);
  }
}

static void transform_greasepencil(GreasePencil &grease_pencil, const float4x4 &transform)
{
  using namespace blender::bke::greasepencil;
  for (const int layer_index : grease_pencil.layers().index_range()) {
    Layer &layer = grease_pencil.layer(layer_index);
    float4x4 local_transform = layer.local_transform();
    local_transform = transform * local_transform;
    layer.set_local_transform(local_transform);
  }
}

static void translate_instances(bke::Instances &instances, const float3 translation)
{
  MutableSpan<float4x4> transforms = instances.transforms_for_write();
  threading::parallel_for(transforms.index_range(), 1024, [&](const IndexRange range) {
    for (float4x4 &instance_transform : transforms.slice(range)) {
      add_v3_v3(instance_transform.ptr()[3], translation);
    }
  });
}

static void transform_instances(bke::Instances &instances, const float4x4 &transform)
{
  MutableSpan<float4x4> transforms = instances.transforms_for_write();
  threading::parallel_for(transforms.index_range(), 1024, [&](const IndexRange range) {
    for (float4x4 &instance_transform : transforms.slice(range)) {
      instance_transform = transform * instance_transform;
    }
  });
}

static void transform_volume(Volume &volume,
                             const float4x4 &transform,
                             TransformGeometryErrors &r_errors)
{
#ifdef WITH_OPENVDB
  openvdb::Mat4s vdb_matrix;
  memcpy(vdb_matrix.asPointer(), &transform, sizeof(float[4][4]));
  openvdb::Mat4d vdb_matrix_d{vdb_matrix};

  const int grids_num = BKE_volume_num_grids(&volume);
  for (const int i : IndexRange(grids_num)) {
    bke::VolumeGridData *volume_grid = BKE_volume_grid_get_for_write(&volume, i);

    float4x4 grid_matrix = bke::volume_grid::get_transform_matrix(*volume_grid);
    grid_matrix = transform * grid_matrix;
    const float determinant = math::determinant(grid_matrix);
    if (!BKE_volume_grid_determinant_valid(determinant)) {
      r_errors.volume_too_small = true;
      /* Clear the tree because it is too small. */
      bke::volume_grid::clear_tree(*volume_grid);
      if (determinant == 0) {
        /* Reset rotation and scale. */
        grid_matrix.x_axis() = float3(1, 0, 0);
        grid_matrix.y_axis() = float3(0, 1, 0);
        grid_matrix.z_axis() = float3(0, 0, 1);
      }
      else {
        /* Keep rotation but reset scale. */
        grid_matrix.x_axis() = math::normalize(grid_matrix.x_axis());
        grid_matrix.y_axis() = math::normalize(grid_matrix.y_axis());
        grid_matrix.z_axis() = math::normalize(grid_matrix.z_axis());
      }
    }
    try {
      bke::volume_grid::set_transform_matrix(*volume_grid, grid_matrix);
    }
    catch (...) {
      r_errors.bad_volume_transform = true;
    }
  }

#else
  UNUSED_VARS(volume, transform, r_errors);
#endif
}

static void translate_volume(Volume &volume, const float3 translation)
{
  TransformGeometryErrors errors;
  transform_volume(volume, math::from_location<float4x4>(translation), errors);
}

static void transform_curve_edit_hints(bke::CurvesEditHints &edit_hints, const float4x4 &transform)
{
  if (const std::optional<MutableSpan<float3>> positions = edit_hints.positions_for_write()) {
    transform_positions(*positions, transform);
  }
  float3x3 deform_mat;
  copy_m3_m4(deform_mat.ptr(), transform.ptr());
  if (edit_hints.deform_mats.has_value()) {
    MutableSpan<float3x3> deform_mats = *edit_hints.deform_mats;
    threading::parallel_for(deform_mats.index_range(), 1024, [&](const IndexRange range) {
      for (const int64_t i : range) {
        deform_mats[i] = deform_mat * deform_mats[i];
      }
    });
  }
  else {
    edit_hints.deform_mats.emplace(edit_hints.curves_id_orig.geometry.point_num, deform_mat);
  }
}

static void transform_grease_pencil_edit_hints(bke::GreasePencilEditHints &edit_hints,
                                               const float4x4 &transform)
{
  if (!edit_hints.drawing_hints) {
    return;
  }

  for (bke::GreasePencilDrawingEditHints &drawing_hints : *edit_hints.drawing_hints) {
    if (const std::optional<MutableSpan<float3>> positions = drawing_hints.positions_for_write()) {
      transform_positions(*positions, transform);
    }
    float3x3 deform_mat = transform.view<3, 3>();
    if (drawing_hints.deform_mats.has_value()) {
      MutableSpan<float3x3> deform_mats = *drawing_hints.deform_mats;
      threading::parallel_for(deform_mats.index_range(), 1024, [&](const IndexRange range) {
        for (const int64_t i : range) {
          deform_mats[i] = deform_mat * deform_mats[i];
        }
      });
    }
    else {
      drawing_hints.deform_mats.emplace(drawing_hints.drawing_orig->strokes().points_num(),
                                        deform_mat);
    }
  }
}

static void transform_gizmo_edit_hints(bke::GizmoEditHints &edit_hints, const float4x4 &transform)
{
  for (float4x4 &m : edit_hints.gizmo_transforms.values()) {
    m = transform * m;
  }
}

static void translate_curve_edit_hints(bke::CurvesEditHints &edit_hints, const float3 &translation)
{
  if (const std::optional<MutableSpan<float3>> positions = edit_hints.positions_for_write()) {
    translate_positions(*positions, translation);
  }
}

static void translate_gizmos_edit_hints(bke::GizmoEditHints &edit_hints, const float3 &translation)
{
  for (float4x4 &m : edit_hints.gizmo_transforms.values()) {
    m.location() += translation;
  }
}

void translate_geometry(bke::GeometrySet &geometry, const float3 translation)
{
  if (math::is_zero(translation)) {
    return;
  }
  if (Curves *curves = geometry.get_curves_for_write()) {
    curves->geometry.wrap().translate(translation);
  }
  if (Mesh *mesh = geometry.get_mesh_for_write()) {
    bke::mesh_translate(*mesh, translation, false);
  }
  if (PointCloud *pointcloud = geometry.get_pointcloud_for_write()) {
    translate_pointcloud(*pointcloud, translation);
  }
  if (GreasePencil *grease_pencil = geometry.get_grease_pencil_for_write()) {
    translate_greasepencil(*grease_pencil, translation);
  }
  if (Volume *volume = geometry.get_volume_for_write()) {
    translate_volume(*volume, translation);
  }
  if (bke::Instances *instances = geometry.get_instances_for_write()) {
    translate_instances(*instances, translation);
  }
  if (bke::CurvesEditHints *curve_edit_hints = geometry.get_curve_edit_hints_for_write()) {
    translate_curve_edit_hints(*curve_edit_hints, translation);
  }
  if (bke::GizmoEditHints *gizmo_edit_hints = geometry.get_gizmo_edit_hints_for_write()) {
    translate_gizmos_edit_hints(*gizmo_edit_hints, translation);
  }
}

std::optional<TransformGeometryErrors> transform_geometry(bke::GeometrySet &geometry,
                                                          const float4x4 &transform)
{
  if (transform == float4x4::identity()) {
    return std::nullopt;
  }
  TransformGeometryErrors errors;
  if (Curves *curves = geometry.get_curves_for_write()) {
    curves->geometry.wrap().transform(transform);
  }
  if (Mesh *mesh = geometry.get_mesh_for_write()) {
    bke::mesh_transform(*mesh, transform, false);
  }
  if (PointCloud *pointcloud = geometry.get_pointcloud_for_write()) {
    transform_pointcloud(*pointcloud, transform);
  }
  if (GreasePencil *grease_pencil = geometry.get_grease_pencil_for_write()) {
    transform_greasepencil(*grease_pencil, transform);
  }
  if (Volume *volume = geometry.get_volume_for_write()) {
    transform_volume(*volume, transform, errors);
  }
  if (bke::Instances *instances = geometry.get_instances_for_write()) {
    transform_instances(*instances, transform);
  }
  if (bke::CurvesEditHints *curve_edit_hints = geometry.get_curve_edit_hints_for_write()) {
    transform_curve_edit_hints(*curve_edit_hints, transform);
  }
  if (bke::GreasePencilEditHints *grease_pencil_edit_hints =
          geometry.get_grease_pencil_edit_hints_for_write())
  {
    transform_grease_pencil_edit_hints(*grease_pencil_edit_hints, transform);
  }
  if (bke::GizmoEditHints *gizmo_edit_hints = geometry.get_gizmo_edit_hints_for_write()) {
    transform_gizmo_edit_hints(*gizmo_edit_hints, transform);
  }

  if (errors.volume_too_small) {
    return errors;
  }
  return std::nullopt;
}

void transform_mesh(Mesh &mesh,
                    const float3 translation,
                    const math::Quaternion rotation,
                    const float3 scale)
{
  const float4x4 matrix = math::from_loc_rot_scale<float4x4>(translation, rotation, scale);
  bke::mesh_transform(mesh, matrix, false);
}

}  // namespace blender::geometry
