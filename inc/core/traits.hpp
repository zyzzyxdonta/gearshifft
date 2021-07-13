#ifndef TRAITS_HPP_
#define TRAITS_HPP_

#include "types.hpp"

namespace gearshifft {

  template <typename T_Precision>
  struct ToString;

  template <>
  struct ToString<float16>  {
    static const char* value() {
      return "float16";
    }
  };

  template <>
  struct ToString<float>  {
    static const char* value() {
      return "float";
    }
  };

  template <>
  struct ToString<double>  {
    static const char* value() {
      return "double";
    }
  };

// SFINAE test if T has title method
  template <typename T>
  class has_title
  {
    using one = char;
    using two = long;
    template <typename C> static one test( decltype(&C::Title) ) ;
    template <typename C> static two test(...);
  public:
    enum { value = sizeof(test<T>(0)) == sizeof(char) };
  };
/**
 * Trait to get precision type of Real or Complex data type.
 */
  template<typename T, bool IsComplex>
  struct Precision;
  template<typename T>
  struct Precision<T, true> { using type = typename T::value_type; };
  template<typename T>
  struct Precision<T, false> { using type = T; };
}
#endif // TRAITS_HPP_
