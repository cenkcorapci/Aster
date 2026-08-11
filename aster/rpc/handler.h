#pragma once

#include <memory>

#include "Aster.h"
#include "aster/server/catalog.h"

namespace aster {
namespace rpc {

// AsterIf implementation backed by a multi-collection Catalog.
class AsterHandler : public AsterIf {
 public:
  explicit AsterHandler(Catalog* catalog);

  void createCollection(const std::string& name) override;
  void configureCollection(const CollectionConfig& config) override;
  void dropCollection(const std::string& name) override;
  void upsert(const std::string& collection, const Document& doc,
              const ConsistencyLevel::type consistency) override;
  void get(Document& _return, const std::string& collection,
           const std::string& id,
           const ConsistencyLevel::type consistency) override;
  void remove(const std::string& collection, const std::string& id,
              const ConsistencyLevel::type consistency) override;
  void search(SearchResponse& _return, const SearchRequest& req) override;

 private:
  Catalog* catalog_;
};

}  // namespace rpc
}  // namespace aster
