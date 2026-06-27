#include "broker_exec/domain/version.hpp"

namespace broker_exec::domain {

std::string_view library_version() noexcept {
  return "0.1.0";
}

}  // namespace broker_exec::domain
