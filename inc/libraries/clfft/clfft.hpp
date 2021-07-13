#ifndef CLFFT_HPP_
#define CLFFT_HPP_

#include "core/options.hpp"
//#include "core/timer_opencl.hpp"
#include "core/fft.hpp"
#include "core/traits.hpp"
#include "core/context.hpp"

#include "clfft_helper.hpp"

#include <clFFT.h>
#include <array>
#include <algorithm>
#include <numeric>
#include <regex>
#include <stdexcept>
#include <boost/algorithm/string/predicate.hpp> // iequals
#include <boost/algorithm/string.hpp> // split, is_any_of

namespace gearshifft
{
  namespace ClFFT
  {
    namespace traits
    {
      template< typename T_Precision >
      struct Types;

      template<>
      struct Types<float>
      {
        using ComplexType = cl_float2;
        using RealType = cl_float;
      };

      template<>
      struct Types<double>
      {
        using ComplexType = cl_double2;
        using RealType = cl_double;
      };

      template< typename TPrecision=float >
      struct FFTPrecision: std::integral_constant< clfftPrecision, CLFFT_SINGLE >{};
      template<>
      struct FFTPrecision<double>: std::integral_constant< clfftPrecision, CLFFT_DOUBLE >{};

      template< bool IsComplex=true >
      struct FFTLayout {
        static constexpr clfftLayout value = CLFFT_COMPLEX_INTERLEAVED;
        static constexpr clfftLayout value_transformed = CLFFT_COMPLEX_INTERLEAVED;
      };
      template<>
      struct FFTLayout<false> {
        static constexpr clfftLayout value = CLFFT_REAL;
        static constexpr clfftLayout value_transformed = CLFFT_HERMITIAN_INTERLEAVED;
      };

      template< bool T_isInplace=true >
      struct FFTInplace: std::integral_constant< clfftResultLocation, CLFFT_INPLACE >{};
      template<>
      struct FFTInplace<false>: std::integral_constant< clfftResultLocation, CLFFT_OUTOFPLACE >{};
    } // traits



    struct ClFFTContextAttributes {
      cl_platform_id platform = 0;
      cl_device_id device = 0;
      cl_device_id device_used = 0;
      cl_context ctx = 0;
      bool use_host_memory = false; // false, if an accelerator with own memory is used

    };

    struct ClFFTContext : public ContextDefault<OptionsDefault, ClFFTContextAttributes> {

      static const std::string title() {
        return "ClFFT";
      }

      static const std::string get_device_list() {
        auto ss = listClDevices();
        return ss.str();
      }

      std::string get_used_device_properties() {
        assert(context().device_used);
        auto ss = getClDeviceInformations(context().device_used);
        return ss.str();
      }

      void create() {
        cl_context_properties props[3] = { CL_CONTEXT_PLATFORM, 0, 0 };
        cl_int err{};
        cl_device_type devtype = CL_DEVICE_TYPE_GPU;

        const std::string options_devtype = options().getDevice();
        std::regex e("^([0-9]+):([0-9]+)$"); // get user specified platform and device id
        if(std::regex_search(options_devtype, e)) {
          std::vector<std::string> token;
          boost::split(token, options_devtype, boost::is_any_of(":"));
          unsigned long id_platform = std::stoul(token[0].c_str());
          unsigned long id_device = std::stoul(token[1].c_str());
          getPlatformAndDeviceByID(&context().platform,
                                   &context().device,
                                   id_platform,
                                   id_device);
        }else{
          if(boost::iequals(options_devtype, "cpu")) { // case insensitive compare
            devtype = CL_DEVICE_TYPE_CPU;
          } else if(boost::iequals(options_devtype, "acc")) {
            devtype = CL_DEVICE_TYPE_ACCELERATOR;
          } else if(boost::iequals(options_devtype, "gpu")) {
            devtype = CL_DEVICE_TYPE_GPU;
          } else {
            throw std::runtime_error("Unsupported device type");
          }
          findClDevice(devtype,
                       &context().platform,
                       &context().device);
        }

        devtype = getDeviceType(context().device);
        if(devtype!=CL_DEVICE_TYPE_GPU && devtype!=CL_DEVICE_TYPE_ACCELERATOR) {
          context().use_host_memory=true;
        }

        context().device_used = context().device;
        props[1] = reinterpret_cast<cl_context_properties>(context().platform);
        if(devtype == CL_DEVICE_TYPE_CPU) { // if only subset of cores is requested
          const size_t ndevs = options().getNumberDevices();
          if(ndevs>0) {
            const cl_device_partition_property properties[3] = {
              CL_DEVICE_PARTITION_BY_COUNTS,
              static_cast<cl_device_partition_property>(ndevs),
              CL_DEVICE_PARTITION_BY_COUNTS_LIST_END
            };
            cl_device_id subdev_id = 0;
            CHECK_CL(clCreateSubDevices(context().device,
                                        properties, 1, &subdev_id, NULL));
            context().device_used = subdev_id;
          }
        }
        context().ctx = clCreateContext( props,
                                         1,
                                         &context().device_used,
                                         nullptr, nullptr, &err );
        CHECK_CL(err);
        clfftSetupData fftSetup;
        CHECK_CL(clfftInitSetupData(&fftSetup));
        CHECK_CL(clfftSetup(&fftSetup));
      }

      void destroy() {
        if(context().ctx) {
          CHECK_CL(clReleaseContext( context().ctx ));
          CHECK_CL(clReleaseDevice( context().device ));
          CHECK_CL( clfftTeardown( ) );
          context().device = 0;
          context().ctx = 0;
        }
      }
    };

    /**
     * ClFFT plan and execution class.
     *
     * This class handles:
     * - {1D, 2D, 3D} x {R2C, C2R, C2C} x {inplace, outplace} x {float, double}.
     */
    template<typename TFFT, // see fft_abstract.hpp (FFT_Inplace_Real, ...)
             typename TPrecision, // double, float
             size_t   NDim // 1..3
             >
    struct ClFFTImpl
    {
      using ComplexType = typename traits::Types<TPrecision>::ComplexType;
      using RealType = typename traits::Types<TPrecision>::RealType;
      using Extent = std::array<size_t,NDim>;
      static constexpr bool IsInplace = TFFT::IsInplace;
      static constexpr bool IsComplex = TFFT::IsComplex;
      static constexpr bool IsInplaceReal = IsInplace && !IsComplex;
      static constexpr clfftDim FFTDim = NDim==1 ? CLFFT_1D : NDim==2 ? CLFFT_2D : CLFFT_3D;
      using value_type = typename std::conditional<IsComplex,ComplexType,RealType>::type;

      ClFFTContextAttributes context_;
      cl_command_queue queue_ = nullptr;
      clfftPlanHandle plan_   = 0;

      /// input buffer
      cl_mem data_            = nullptr;
      /// intermediate output buffer (transformed input values)
      cl_mem data_complex_    = nullptr;
      /// size in bytes of FFT input data
      size_t data_size_         = 0;
      /// size in bytes of FFT(input) for out-of-place transforms
      size_t data_complex_size_ = 0;

      /// extents of the FFT input data
      Extent extents_   = {{0}};
      /// extents of the FFT complex data (=FFT(input))
      Extent extents_complex_ = {{0}};
      /// product of corresponding extents
      size_t n_         = 0;
      /// product of corresponding extents
      size_t n_complex_ = 0;

      /// FFT extents for OpenCL clFFT
      std::array<size_t, 3> extents_cl_ = {{0}};

      /// strides setting in input data, for clfft kernel
      size_t strides_[3] = {1};
      /// strides setting in output data, for clfft kernel
      size_t strides_complex_[3] = {1};
      /// total length of input values, for clfft kernel
      size_t dist_;
      /// total length of output values, for clfft kernel
      size_t dist_complex_;
      // for padding in memory transfers
      size_t pitch_;
      size_t region_[3] = {0};
      size_t offset_[3] = {0};


      ClFFTImpl(const Extent& cextents) {
        cl_int err{};
        context_ = ClFFTContext::context();
        if(context_.ctx==0) {
          throw std::runtime_error("Context has not been created.");
        }

        extents_ = interpret_as::row_major(cextents);
        n_ = std::accumulate(extents_.begin(),
                             extents_.end(),
                             1,
                             std::multiplies<size_t>());
        std::copy(extents_.begin(), extents_.end(), extents_cl_.begin());

        extents_complex_ = extents_;
        if(!IsComplex){
          extents_complex_.front() = (extents_.front()/2 + 1);
        }

        n_complex_ = std::accumulate(extents_complex_.begin(),
                                     extents_complex_.end(),
                                     1,
                                     std::multiplies<size_t>());

        data_size_ = (IsInplaceReal ? 2*n_complex_ : n_) * sizeof(value_type);
        if(!IsInplace) {
          data_complex_size_ = n_complex_ * sizeof(ComplexType);
        }


        // check supported sizes : http://clmathlibraries.github.io/clFFT/
        if((std::is_same<TPrecision,float>::value && n_>(1<<24) && !IsComplex)
           ||
           (std::is_same<TPrecision,double>::value && n_>(1<<22) && !IsComplex)
           ||
           (std::is_same<TPrecision,float>::value && n_>(1<<27) && IsComplex)
           ||
           (std::is_same<TPrecision,double>::value && n_>(1<<26) && IsComplex)) {
          throw std::runtime_error("Unsupported lengths.");
        }

        queue_ = clCreateCommandQueue( context_.ctx, context_.device_used, 0, &err );
//        queue_ = clCreateCommandQueue( context_.ctx, context_.device_used, CL_QUEUE_PROFILING_ENABLE, &err );
        CHECK_CL(err);


        // strides_ for clfft kernel, if less than 3D, strides_ will be ignored
        if(IsInplaceReal) {
          strides_[1] = 2 * (extents_[0]/2+1);
          strides_[2] = 2 * n_complex_ / extents_[NDim-1];
          dist_       = 2 * n_complex_;
        }else{
          strides_[1] = extents_[0];
          strides_[2] = n_ / extents_[NDim-1];
          dist_       = n_;
        }

        strides_complex_[1] = extents_complex_[0];
        strides_complex_[2] = n_complex_ / extents_complex_[NDim-1];
        dist_complex_       = n_complex_;

        // if memory transfer must pad reals
        if(IsInplaceReal && NDim>1) {
          size_t width  = extents_[0] * sizeof(RealType);
          size_t height = n_ / extents_[0];
          pitch_ = (extents_[0]/2+1) * sizeof(ComplexType);
          region_[0] = width; // in bytes
          region_[1] = height; // in counts (OpenCL1.1 is wrong here speaking of bytes)
          region_[2] = 1; // in counts (same)
        }
      }

      ~ClFFTImpl() {
        destroy();
      }

      /**
       * Returns size in bytes of one data transfer.
       *
       * Upload and download have the same size due to round-trip FFT.
       * \return Size in bytes of FFT data to be transferred (to device or to host memory buffer).
       */
      size_t get_transfer_size() {
        return IsInplaceReal ? n_*sizeof(RealType) : data_size_;
      }

      /**
       * Returns allocated memory on device for FFT
       */
      size_t get_allocation_size() {
        return data_size_ + data_complex_size_;
      }

      /**
       * Returns estimated allocated memory on device for FFT plan
       */
      size_t get_plan_size() {
        size_t gmemsize = 95*static_cast<size_t>(getMaxGlobalMemSize(context_.device))/100;
        size_t wanted = data_size_ + data_complex_size_;
        size_t size1 = 0;
        size_t size2 = 0;

        if(context_.use_host_memory) {
          wanted += 2*data_size_; // also consider already located data of benchmark in RAM
        }

        if( gmemsize > wanted ) {
          init_forward();
          CHECK_CL(clfftGetTmpBufSize( plan_, &size1 ));
          init_inverse();
          CHECK_CL(clfftGetTmpBufSize( plan_, &size2 ));
          CHECK_CL(clfftDestroyPlan( &plan_ ));
          wanted += std::max(size1,size2);
        }

        if( gmemsize <= wanted ) {
          std::stringstream ss;
          ss << gmemsize << "<" << wanted << " (bytes)";
          throw std::runtime_error("FFT plan + data are exceeding [global] memory. "+ss.str());
        }

        return std::max(size1,size2);
      }

      // --- next methods are benchmarked ---

      /**
       * Allocate buffers on OpenCL device
       */
      void allocate() {
        cl_int err{};
        data_ = clCreateBuffer( context_.ctx,
                                CL_MEM_READ_WRITE,
                                data_size_,
                                nullptr, // host pointer @todo
                                &err );
        if(!IsInplace){
          data_complex_ = clCreateBuffer( context_.ctx,
                                          CL_MEM_READ_WRITE,
                                          data_complex_size_,
                                          nullptr, // host pointer
                                          &err );
        }
      }

      /**
       * Create clfft forward plan with layout, precision, strides and distances
       */
      void init_forward() {
        CHECK_CL(clfftCreateDefaultPlan(&plan_, context_.ctx, FFTDim, extents_cl_.data()));
        CHECK_CL(clfftSetPlanPrecision(plan_, traits::FFTPrecision<TPrecision>::value));
        CHECK_CL(clfftSetLayout(plan_,
                                traits::FFTLayout<IsComplex>::value,
                                traits::FFTLayout<IsComplex>::value_transformed));
        CHECK_CL(clfftSetResultLocation(plan_, traits::FFTInplace<IsInplace>::value));
        CHECK_CL(clfftSetPlanInStride(plan_, FFTDim, strides_));
        CHECK_CL(clfftSetPlanOutStride(plan_, FFTDim, strides_complex_));
        CHECK_CL(clfftSetPlanDistance(plan_, dist_, dist_complex_));
        CHECK_CL(clfftBakePlan(plan_,
                               1, // number of queues
                               &queue_,
                               nullptr, // callback
                               nullptr)); // user data
        CHECK_CL( clFinish(queue_) );
      }


      /**
       * If real-transform: create clfft backward plan with layout, precision, strides and distances
       */
      void init_inverse() {
        if(!IsComplex){
          CHECK_CL(clfftDestroyPlan( &plan_ ));
          CHECK_CL(clfftCreateDefaultPlan(&plan_, context_.ctx, FFTDim, extents_cl_.data()));
          CHECK_CL(clfftSetPlanPrecision(plan_, traits::FFTPrecision<TPrecision>::value));
          CHECK_CL(clfftSetLayout(plan_,
                                  traits::FFTLayout<IsComplex>::value_transformed,
                                  traits::FFTLayout<IsComplex>::value));
          CHECK_CL(clfftSetPlanOutStride(plan_, FFTDim, strides_));
          CHECK_CL(clfftSetPlanInStride(plan_, FFTDim, strides_complex_));
          CHECK_CL(clfftSetPlanDistance(plan_, dist_complex_, dist_));
          CHECK_CL(clfftBakePlan(plan_,
                                 1, // number of queues
                                 &queue_,
                                 0, // callback
                                 0)); // user data
        }
        CHECK_CL( clFinish(queue_) );
      }

      void execute_forward() {
        CHECK_CL(clfftEnqueueTransform(plan_,
                                       CLFFT_FORWARD,
                                       1, // numQueuesAndEvents
                                       &queue_,
                                       0, // numWaitEvents
                                       0, // waitEvents
                                       nullptr,//context_.event.get(), // outEvents
                                       &data_,  // input
                                       IsInplace ? &data_ : &data_complex_, // output
                                       0)); // tmpBuffer
        CHECK_CL( clFinish(queue_) );
      }

      void execute_inverse() {
        CHECK_CL(clfftEnqueueTransform(plan_,
                                       CLFFT_BACKWARD,
                                       1, // numQueuesAndEvents
                                       &queue_,
                                       0, // numWaitEvents
                                       nullptr, // waitEvents
                                       nullptr,//context_.event.get(), // outEvents
                                       IsInplace ? &data_ : &data_complex_, // input
                                       &data_, // output
                                       nullptr)); // tmpBuffer
        CHECK_CL( clFinish(queue_) );
      }

      template<typename THostData>
      void upload(THostData* input) {
        if(IsInplaceReal && NDim>1) {
          CHECK_CL(clEnqueueWriteBufferRect( queue_,
                                             data_,
                                             CL_FALSE, // blocking_write
                                             offset_, // buffer origin
                                             offset_, // host origin
                                             region_,
                                             pitch_, // buffer row pitch
                                             0, // buffer slice pitch
                                             0, // host row pitch
                                             0, // host slice pitch
                                             input,
                                             0, // num_events_in_wait_list
                                             nullptr, // event_wait_list
                                             nullptr ));//context_.event.get() )); // event
        }else{
          CHECK_CL(clEnqueueWriteBuffer( queue_,
                                         data_,
                                         CL_FALSE, // blocking_write
                                         0, // offset
                                         get_transfer_size(),
                                         input,
                                         0, // num_events_in_wait_list
                                         nullptr, // event_wait_list
                                         nullptr));//context_.event.get() )); // event
        }
        CHECK_CL( clFinish(queue_) );
      }

      template<typename THostData>
      void download(THostData* output) {
        if(IsInplaceReal && NDim>1) {
          CHECK_CL(clEnqueueReadBufferRect( queue_,
                                            data_,
                                            CL_FALSE, // blocking_write
                                            offset_, // buffer origin
                                            offset_, // host origin
                                            region_,
                                            pitch_, // buffer row pitch
                                            0, // buffer slice pitch
                                            0, // host row pitch
                                            0, // host slice pitch
                                            output,
                                            0, // num_events_in_wait_list
                                            nullptr, // event_wait_list
                                            nullptr));//context_.event.get() )); // event
        }else{
          CHECK_CL(clEnqueueReadBuffer( queue_,
                                        data_,
                                        CL_FALSE, // blocking_write
                                        0, // offset
                                        get_transfer_size(),
                                        output,
                                        0, // num_events_in_wait_list
                                        nullptr, // event_wait_list
                                        nullptr));//context_.event.get() )); // event
        }
        CHECK_CL( clFinish(queue_) );
      }

      void destroy() {
        if(queue_) {
          CHECK_CL( clFinish(queue_) );
          if( data_ ) {
            CHECK_CL( clReleaseMemObject( data_ ) );
            data_ = 0;
            if(!IsInplace && data_complex_){
              CHECK_CL( clReleaseMemObject( data_complex_ ) );
              data_complex_ = 0;
            }
          }
          if(plan_) {
            CHECK_CL(clfftDestroyPlan( &plan_ ));
            plan_ = 0;
          }
          CHECK_CL( clReleaseCommandQueue( queue_ ) );
          queue_ = 0;
        }
      }
    };

/*  // OpenCL timer seems to be not reliable, so for proper timings we stick with CPU timer
    typedef gearshifft::FFT<gearshifft::FFT_Inplace_Real, ClFFTImpl, TimerGPU<Context> > Inplace_Real;
    typedef gearshifft::FFT<gearshifft::FFT_Outplace_Real, ClFFTImpl, TimerGPU<Context> > Outplace_Real;
    typedef gearshifft::FFT<gearshifft::FFT_Inplace_Complex, ClFFTImpl, TimerGPU<Context> > Inplace_Complex;
    typedef gearshifft::FFT<gearshifft::FFT_Outplace_Complex, ClFFTImpl, TimerGPU<Context> > Outplace_Complex;*/

    using Inplace_Real = gearshifft::FFT<FFT_Inplace_Real,
                                         FFT_Plan_Reusable,
                                         ClFFTImpl,
                                         TimerCPU >;
    using Outplace_Real = gearshifft::FFT<FFT_Outplace_Real,
                                          FFT_Plan_Reusable,
                                          ClFFTImpl,
                                          TimerCPU >;
    using Inplace_Complex = gearshifft::FFT<FFT_Inplace_Complex,
                                            FFT_Plan_Reusable,
                                            ClFFTImpl,
                                            TimerCPU >;
    using Outplace_Complex = gearshifft::FFT<FFT_Outplace_Complex,
                                             FFT_Plan_Reusable,
                                             ClFFTImpl,
                                             TimerCPU >;

  } // namespace ClFFT
} // gearshifft



#endif /* CLFFT_HPP_ */
