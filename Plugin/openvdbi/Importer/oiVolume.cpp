#include "pch.h"
#include "oiInternal.h"
#include "oiContext.h"
#include "oiVolume.h"

#include "OpenVDBImporter.h"

#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#include <openvdb/tools/Interpolation.h>

template <typename RealType>
struct ValueRange {
public:
    ValueRange()
            : m_min(std::numeric_limits<RealType>::max())
            , m_max(std::numeric_limits<RealType>::min())
    {}
    ValueRange(RealType min_, RealType max_)
            : m_min(min_)
            , m_max(max_)
    {}

    RealType getMin() const { return m_min; }
    RealType getMax() const { return m_max; }

    void addValue(RealType value)
    {
        m_min = std::min(m_min, value);
        m_max = std::max(m_max, value);
    }

private:
    RealType m_min, m_max;
};

typedef ValueRange<float> FloatRange;

enum class FilterMode { BOX, MULTIRES, AUTO };

template <typename T>
struct identity { using type = T; };

template <typename T> inline
T unlerp(typename identity<T>::type a, typename identity<T>::type b, T x)
{
    return (x - a) / (b - a);
}

inline openvdb::CoordBBox getIndexSpaceBoundingBox(const openvdb::GridBase& grid)
{
    try
    {
        const auto file_bbox_min = openvdb::Coord(grid.metaValue<openvdb::Vec3i>("file_bbox_min"));
        if (file_bbox_min.x() == std::numeric_limits<int>::max() ||
            file_bbox_min.y() == std::numeric_limits<int>::max() ||
            file_bbox_min.z() == std::numeric_limits<int>::max()) {
            return {};
        }
        const auto file_bbox_max = openvdb::Coord(grid.metaValue<openvdb::Vec3i>("file_bbox_max"));

        if (file_bbox_max.x() == std::numeric_limits<int>::min() ||
            file_bbox_max.y() == std::numeric_limits<int>::min() ||
            file_bbox_max.z() == std::numeric_limits<int>::min()) {
            return {};
        }

        return { file_bbox_min, file_bbox_max };
    }
    catch (openvdb::Exception e)
    {
        return {};
    }
}

template <typename SamplingFunc, typename RealType>
bool sampleVolume( const openvdb::Coord& extents, SamplingFunc sampling_func, FloatRange& out_value_range, RealType* out_samples)
{
    const auto domain = openvdb::CoordBBox(openvdb::Coord(0, 0, 0), extents - openvdb::Coord(1, 1, 1));
    if (domain.empty())
    {
        return false;
    }
    const auto num_voxels = domain.volume();

    // Sample on a lattice.
    typedef tbb::enumerable_thread_specific<FloatRange> PerThreadRange;
    PerThreadRange ranges;
    const openvdb::Vec3i stride = {1, extents.x(), extents.x() * extents.y()};
    tbb::atomic<bool> cancelled;
    cancelled = false;
    tbb::parallel_for(
            domain,
            [&sampling_func, &stride, &ranges, out_samples, &cancelled] (const openvdb::CoordBBox& bbox)
            {
                const auto local_extents = bbox.extents();

                // Loop through local bbox.
                PerThreadRange::reference this_thread_range = ranges.local();
                for (auto z = bbox.min().z(); z <= bbox.max().z(); ++z)
                {
                    for (auto y = bbox.min().y(); y <= bbox.max().y(); ++y)
                    {
                        for (auto x = bbox.min().x(); x <= bbox.max().x(); ++x)
                        {
                            const auto domain_index = openvdb::Vec3i(x, y, z);
                            const auto linear_index = domain_index.dot(stride) * 4;
                            const auto sample_value = sampling_func(domain_index);

                            // fixme
                            out_samples[linear_index + 0] = sample_value;
                            out_samples[linear_index + 1] = sample_value;
                            out_samples[linear_index + 2] = sample_value;
                            out_samples[linear_index + 3] = sample_value;
                            this_thread_range.addValue(sample_value);
                        }
                    }
                }
            });

    // Merge per-thread value ranges.
    out_value_range = FloatRange();
    for (const FloatRange& per_thread_range : ranges) {
        out_value_range.addValue(per_thread_range.getMin());
        out_value_range.addValue(per_thread_range.getMax());
    }

    // Remap sample values to [0, 1].
    int size = num_voxels * 4;
    typedef tbb::blocked_range<size_t> tbb_range;
    tbb::parallel_for(tbb_range(0, size), [out_samples, &out_value_range](const tbb_range& range)
    {
        for (auto i = range.begin(); i < range.end(); ++i)
        {
            out_samples[i] = unlerp( out_value_range.getMin(), out_value_range.getMax(), out_samples[i]);
        }
    });
}

template <typename RealType>
bool sampleGrid(
        const openvdb::FloatGrid& grid,
        const openvdb::Coord& sampling_extents,
        FloatRange& value_range,
        openvdb::Vec3d& scale,
        RealType* out_data)
{
    assert(out_data);

    const auto grid_bbox_is = getIndexSpaceBoundingBox(grid);
    const auto bbox_world = grid.transform().indexToWorld(grid_bbox_is);

    scale = bbox_world.extents();

    // Return if the grid bbox is empty.
    if (grid_bbox_is.empty())
    {
        return false;
    }

    const auto domain_extents = sampling_extents.asVec3d();
    openvdb::tools::GridSampler<openvdb::FloatGrid, openvdb::tools::BoxSampler> sampler(grid);

    auto sampling_func = [&sampler, &bbox_world, &domain_extents] (const openvdb::Vec3d& domain_index) -> RealType
    {
        const auto sample_pos_ws = bbox_world.min() + (domain_index + 0.5) / domain_extents * bbox_world.extents();
        return sampler.wsSample(sample_pos_ws);
    };

    sampleVolume( sampling_extents, sampling_func, value_range, out_data);
}

oiVolume::oiVolume(const openvdb::FloatGrid& grid, const openvdb::Coord& extents)
    : m_grid(grid), m_extents(extents), m_scaleFactor(1.0f)
{
    grid.print();

    int voxel_count = extents.x() * extents.y() * extents.z();
    int texture_format = 20;
    m_summary = new oiVolumeSummary(voxel_count, extents.x(), extents.y(), extents.z(), texture_format);
}

oiVolume::~oiVolume()
{
}

void oiVolume::reset()
{
}

void oiVolume::setScaleFactor(float scaleFactor)
{
    m_scaleFactor = scaleFactor;
}

void oiVolume::fillTextureBuffer(oiVolumeData& data) const
{
    DebugLog("oiVolume::fillTextureBuffer start");

    if(!data.voxels)
    {
        DebugLog("oiVolume::fillTextureBuffer voxels pointer is null");
        return;
    }

    openvdb::Coord extents{m_summary->width, m_summary->height, m_summary->depth};

    FloatRange value_range;
    openvdb::Vec3d scale;
    sampleGrid(m_grid, extents, value_range, scale, (float*)data.voxels);
    m_summary->min_value = value_range.getMin();
    m_summary->max_value = value_range.getMax();

    m_summary->x_scale = scale.x() * m_scaleFactor;
    m_summary->y_scale = scale.y() * m_scaleFactor;
    m_summary->z_scale = scale.z() * m_scaleFactor;

    DebugLog("scale.x()=%f, scale.y()=%f, scale.z()=%f",
             scale.x(),
             scale.y(),
             scale.z()
    );
    DebugLog("min=%f, max=%f", value_range.getMin(), value_range.getMax());
}

const oiVolumeSummary& oiVolume::getSummary() const
{
    return *m_summary;
}