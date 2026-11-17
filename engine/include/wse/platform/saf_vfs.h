// Platform seam: Storage Access Framework (SAF) virtual file system and Warband module scanner.
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.5 layout
// and carries no PE/PDB citations. Builds a one-time case-insensitive tree index
// over an opaque-document-ID tree (`SafTreeBackend`), avoiding per-open Android
// DocumentsContract binder IPC round-trips (docs/PLATFORM_PLAN.md, slice P2).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wse/game/module_loader_common.h"
#include "wse/game/script.h"
#include "wse/io/text_stream.h"
#include "wse/platform/vfs.h"

namespace wse::platform {

struct SafChildEntry {
    std::string document_id{};
    std::string display_name{};
    bool is_directory = false;
    std::uint64_t size_bytes = 0;
};

// Abstract document-ID backend behind `SafVfs`.
// Implementations wrap Android `DocumentsContract` / `ContentResolver` (via JNI)
// or an in-memory tree (`MemorySafBackend`) for host testing.
class SafTreeBackend {
public:
    virtual ~SafTreeBackend() = default;

    [[nodiscard]] virtual std::string root_document_id() const = 0;

    [[nodiscard]] virtual bool list_children(std::string_view parent_document_id,
                                             std::vector<SafChildEntry>& out_children) = 0;

    [[nodiscard]] virtual bool read_document(std::string_view document_id,
                                             std::vector<std::uint8_t>& out) = 0;

    // Writes bytes to `existing_document_id` if non-empty, or creates/overwrites
    // `display_name` under `parent_document_id`. Writes the resulting document ID
    // into `out_document_id`.
    [[nodiscard]] virtual bool write_document(std::string_view parent_document_id,
                                              std::string_view existing_document_id,
                                              std::string_view display_name,
                                              std::span<const std::uint8_t> data,
                                              std::string& out_document_id) = 0;

    [[nodiscard]] virtual bool create_directory(std::string_view parent_document_id,
                                                std::string_view display_name,
                                                std::string& out_document_id) = 0;

    [[nodiscard]] virtual bool delete_document(std::string_view document_id) = 0;

    // Renames a document in place (`DocumentsContract.renameDocument`), writing its possibly
    // new document ID into `out_document_id`. False when unsupported or on failure.
    [[nodiscard]] virtual bool rename_document(std::string_view /*document_id*/,
                                               std::string_view /*new_name*/,
                                               std::string& out_document_id) {
        out_document_id.clear();
        return false;
    }

    // Moves a document between two directories of the tree (`DocumentsContract.moveDocument`,
    // API 24), writing its new document ID into `out_document_id`. False when unsupported.
    [[nodiscard]] virtual bool move_document(std::string_view /*document_id*/,
                                             std::string_view /*source_parent_id*/,
                                             std::string_view /*target_parent_id*/,
                                             std::string& out_document_id) {
        out_document_id.clear();
        return false;
    }
};

// In-memory document-ID backend for host tests and benchmarks.
// Exposes call counters so tests can verify `SafVfs` resolves reads, writes, and
// directory listings solely from its one-time index without re-querying children.
class MemorySafBackend final : public SafTreeBackend {
public:
    MemorySafBackend() { reset(); }

    void reset() {
        nodes_.clear();
        next_id_ = 1;
        reset_call_counts();
        Node root{};
        root.document_id = "doc:0";
        root.parent_document_id.clear();
        root.display_name.clear();
        root.is_directory = true;
        nodes_.emplace(root.document_id, std::move(root));
    }

    void reset_call_counts() noexcept {
        list_children_calls = 0;
        read_document_calls = 0;
        write_document_calls = 0;
        create_directory_calls = 0;
        delete_document_calls = 0;
        rename_document_calls = 0;
        move_document_calls = 0;
    }

    [[nodiscard]] std::string root_document_id() const override { return "doc:0"; }

    [[nodiscard]] bool list_children(std::string_view parent_document_id,
                                     std::vector<SafChildEntry>& out_children) override {
        ++list_children_calls;
        out_children.clear();
        const auto it = nodes_.find(std::string(parent_document_id));
        if (it == nodes_.end() || !it->second.is_directory) {
            return false;
        }
        out_children.reserve(it->second.children.size());
        for (const std::string& child_id : it->second.children) {
            const auto child_it = nodes_.find(child_id);
            if (child_it == nodes_.end()) {
                continue;
            }
            const Node& child = child_it->second;
            out_children.push_back(SafChildEntry{
                child.document_id,
                child.display_name,
                child.is_directory,
                static_cast<std::uint64_t>(child.bytes.size()),
            });
        }
        return true;
    }

    [[nodiscard]] bool read_document(std::string_view document_id,
                                     std::vector<std::uint8_t>& out) override {
        ++read_document_calls;
        out.clear();
        const auto it = nodes_.find(std::string(document_id));
        if (it == nodes_.end() || it->second.is_directory) {
            return false;
        }
        out = it->second.bytes;
        return true;
    }

    [[nodiscard]] bool write_document(std::string_view parent_document_id,
                                      std::string_view existing_document_id,
                                      std::string_view display_name,
                                      std::span<const std::uint8_t> data,
                                      std::string& out_document_id) override {
        ++write_document_calls;
        out_document_id.clear();
        if (!existing_document_id.empty()) {
            const auto it = nodes_.find(std::string(existing_document_id));
            if (it == nodes_.end() || it->second.is_directory) {
                return false;
            }
            it->second.bytes.assign(data.begin(), data.end());
            out_document_id = it->second.document_id;
            return true;
        }

        if (display_name.empty() || !is_valid_component(display_name)) {
            return false;
        }
        const auto parent_it = nodes_.find(std::string(parent_document_id));
        if (parent_it == nodes_.end() || !parent_it->second.is_directory) {
            return false;
        }
        const std::string wanted = ascii_lower(display_name);
        for (const std::string& cid : parent_it->second.children) {
            const auto cit = nodes_.find(cid);
            if (cit != nodes_.end() && ascii_lower(cit->second.display_name) == wanted) {
                if (cit->second.is_directory) {
                    return false;
                }
                cit->second.bytes.assign(data.begin(), data.end());
                out_document_id = cit->second.document_id;
                return true;
            }
        }

        Node file{};
        file.document_id = allocate_id();
        file.parent_document_id = std::string(parent_document_id);
        file.display_name = std::string(display_name);
        file.is_directory = false;
        file.bytes.assign(data.begin(), data.end());
        out_document_id = file.document_id;
        parent_it->second.children.push_back(file.document_id);
        nodes_.emplace(file.document_id, std::move(file));
        return true;
    }

    [[nodiscard]] bool create_directory(std::string_view parent_document_id,
                                        std::string_view display_name,
                                        std::string& out_document_id) override {
        ++create_directory_calls;
        out_document_id.clear();
        if (display_name.empty() || !is_valid_component(display_name)) {
            return false;
        }
        const auto parent_it = nodes_.find(std::string(parent_document_id));
        if (parent_it == nodes_.end() || !parent_it->second.is_directory) {
            return false;
        }
        const std::string wanted = ascii_lower(display_name);
        for (const std::string& cid : parent_it->second.children) {
            const auto cit = nodes_.find(cid);
            if (cit != nodes_.end() && ascii_lower(cit->second.display_name) == wanted) {
                return false;
            }
        }

        Node dir{};
        dir.document_id = allocate_id();
        dir.parent_document_id = std::string(parent_document_id);
        dir.display_name = std::string(display_name);
        dir.is_directory = true;
        out_document_id = dir.document_id;
        parent_it->second.children.push_back(dir.document_id);
        nodes_.emplace(dir.document_id, std::move(dir));
        return true;
    }

    [[nodiscard]] bool delete_document(std::string_view document_id) override {
        ++delete_document_calls;
        if (document_id == "doc:0") {
            return false;
        }
        const auto it = nodes_.find(std::string(document_id));
        if (it == nodes_.end()) {
            return false;
        }
        if (it->second.is_directory && !it->second.children.empty()) {
            return false;
        }
        const auto parent_it = nodes_.find(it->second.parent_document_id);
        if (parent_it != nodes_.end()) {
            auto& kids = parent_it->second.children;
            kids.erase(std::remove(kids.begin(), kids.end(), it->second.document_id), kids.end());
        }
        nodes_.erase(it);
        return true;
    }

    [[nodiscard]] bool rename_document(std::string_view document_id,
                                       std::string_view new_name,
                                       std::string& out_document_id) override {
        ++rename_document_calls;
        out_document_id.clear();
        const auto it = nodes_.find(std::string(document_id));
        if (it == nodes_.end() || document_id == "doc:0" || new_name.empty() ||
            !is_valid_component(new_name) || has_child_named(it->second.parent_document_id, new_name)) {
            return false;
        }
        it->second.display_name = std::string(new_name);
        out_document_id = it->second.document_id;
        return true;
    }

    [[nodiscard]] bool move_document(std::string_view document_id,
                                     std::string_view source_parent_id,
                                     std::string_view target_parent_id,
                                     std::string& out_document_id) override {
        ++move_document_calls;
        out_document_id.clear();
        const auto it = nodes_.find(std::string(document_id));
        const auto dst = nodes_.find(std::string(target_parent_id));
        if (it == nodes_.end() || dst == nodes_.end() || !dst->second.is_directory ||
            it->second.parent_document_id != source_parent_id ||
            has_child_named(dst->first, it->second.display_name)) {
            return false;
        }
        const auto src = nodes_.find(std::string(source_parent_id));
        if (src != nodes_.end()) {
            auto& kids = src->second.children;
            kids.erase(std::remove(kids.begin(), kids.end(), it->first), kids.end());
        }
        dst->second.children.push_back(it->first);
        it->second.parent_document_id = std::string(target_parent_id);
        out_document_id = it->first;
        return true;
    }

    // Populates this document-ID tree recursively from `source` under `root_rel`.
    [[nodiscard]] bool seed_from_vfs(Vfs& source, std::string_view root_rel = "") {
        return seed_dir(source, root_rel, "doc:0");
    }

    std::size_t list_children_calls = 0;
    std::size_t read_document_calls = 0;
    std::size_t write_document_calls = 0;
    std::size_t create_directory_calls = 0;
    std::size_t delete_document_calls = 0;
    std::size_t rename_document_calls = 0;
    std::size_t move_document_calls = 0;

private:
    [[nodiscard]] bool has_child_named(const std::string& parent_id, std::string_view name) const {
        const auto p = nodes_.find(parent_id);
        if (p == nodes_.end()) {
            return false;
        }
        for (const std::string& kid : p->second.children) {
            const auto k = nodes_.find(kid);
            if (k != nodes_.end() && k->second.display_name == name) {
                return true;
            }
        }
        return false;
    }

    struct Node {
        std::string document_id{};
        std::string parent_document_id{};
        std::string display_name{};
        bool is_directory = false;
        std::vector<std::uint8_t> bytes{};
        std::vector<std::string> children{};
    };

    static bool is_valid_component(std::string_view name) noexcept {
        return name != "." && name != ".." &&
               name.find('/') == std::string_view::npos &&
               name.find('\\') == std::string_view::npos &&
               name.find(':') == std::string_view::npos &&
               name.find('\0') == std::string_view::npos;
    }

    std::string allocate_id() {
        return std::string("doc:") + std::to_string(next_id_++);
    }

    bool seed_dir(Vfs& source, std::string_view src_rel, std::string_view dst_doc_id) {
        std::vector<VfsEntry> entries;
        if (!source.list_dir(src_rel, entries)) {
            return false;
        }
        for (const VfsEntry& entry : entries) {
            const std::string child_rel = game::vfs_join_path(src_rel, entry.name);
            if (entry.is_directory) {
                std::string child_doc_id;
                if (!create_directory(dst_doc_id, entry.name, child_doc_id)) {
                    return false;
                }
                if (!seed_dir(source, child_rel, child_doc_id)) {
                    return false;
                }
            } else {
                std::vector<std::uint8_t> bytes;
                if (!source.read_file(child_rel, bytes)) {
                    return false;
                }
                std::string child_doc_id;
                if (!write_document(dst_doc_id, {}, entry.name, bytes, child_doc_id)) {
                    return false;
                }
            }
        }
        return true;
    }

    std::unordered_map<std::string, Node> nodes_{};
    std::uint64_t next_id_ = 1;
};

// Case-insensitive Vfs over a `SafTreeBackend` using a one-time tree index.
class SafVfs final : public Vfs {
public:
    explicit SafVfs(std::shared_ptr<SafTreeBackend> backend, bool build_index_now = true)
        : backend_(std::move(backend)) {
        if (build_index_now) {
            (void)rebuild_index();
        }
    }

    // Rebuilds the case-insensitive path -> document_id index in one breadth-first walk.
    bool rebuild_index() {
        index_.clear();
        index_complete_ = false;
        file_count_ = 0;
        dir_count_ = 0;

        if (!backend_) {
            return false;
        }
        const std::string root_id = backend_->root_document_id();
        if (root_id.empty()) {
            return false;
        }

        IndexNode root{};
        root.document_id = root_id;
        root.is_directory = true;
        index_.emplace("", std::move(root));

        struct PendingDir {
            std::string norm;
            std::string doc_id;
            std::string orig_rel;
        };
        std::deque<PendingDir> queue;
        std::unordered_set<std::string> visited_docs;
        visited_docs.insert(root_id);
        queue.push_back(PendingDir{"", root_id, ""});

        std::vector<SafChildEntry> children;
        while (!queue.empty()) {
            PendingDir cur = std::move(queue.front());
            queue.pop_front();

            children.clear();
            if (!backend_->list_children(cur.doc_id, children)) {
                index_.clear();
                file_count_ = 0;
                dir_count_ = 0;
                return false;
            }

            std::sort(children.begin(), children.end(), [](const SafChildEntry& a, const SafChildEntry& b) {
                const std::string la = ascii_lower(a.display_name);
                const std::string lb = ascii_lower(b.display_name);
                if (la != lb) {
                    return la < lb;
                }
                return a.display_name < b.display_name;
            });

            std::vector<std::string> child_keys;
            child_keys.reserve(children.size());

            for (const SafChildEntry& child : children) {
                if (!is_safe_child_name(child.display_name) || child.document_id.empty()) {
                    continue;
                }
                const std::string child_norm =
                    cur.norm.empty() ? ascii_lower(child.display_name)
                                     : (cur.norm + "/" + ascii_lower(child.display_name));
                if (index_.count(child_norm) != 0) {
                    continue;
                }
                const std::string child_orig_rel =
                    cur.orig_rel.empty() ? child.display_name
                                         : (cur.orig_rel + "/" + child.display_name);

                IndexNode node{};
                node.document_id = child.document_id;
                node.parent_document_id = cur.doc_id;
                node.original_name = child.display_name;
                node.original_rel_path = child_orig_rel;
                node.is_directory = child.is_directory;
                node.size_bytes = child.size_bytes;
                index_.emplace(child_norm, std::move(node));
                child_keys.push_back(child_norm);

                if (child.is_directory) {
                    ++dir_count_;
                    if (visited_docs.insert(child.document_id).second) {
                        queue.push_back(PendingDir{child_norm, child.document_id, child_orig_rel});
                    }
                } else {
                    ++file_count_;
                }
            }

            auto parent_it = index_.find(cur.norm);
            if (parent_it != index_.end()) {
                parent_it->second.children_keys = std::move(child_keys);
            }
        }

        index_complete_ = true;
        return true;
    }

    [[nodiscard]] bool index_complete() const noexcept { return index_complete_; }
    [[nodiscard]] std::size_t index_file_count() const noexcept { return file_count_; }
    [[nodiscard]] std::size_t index_dir_count() const noexcept { return dir_count_; }

    [[nodiscard]] bool read_file(std::string_view path, std::vector<std::uint8_t>& out) override {
        out.clear();
        if (!backend_ || !index_complete_) {
            return false;
        }
        const auto key = normalize_path(path);
        if (!key) {
            return false;
        }
        const auto it = index_.find(*key);
        if (it == index_.end() || it->second.is_directory) {
            return false;
        }
        return backend_->read_document(it->second.document_id, out);
    }

    [[nodiscard]] bool write_file(std::string_view path,
                                  std::span<const std::uint8_t> data) override {
        if (!backend_ || !index_complete_) {
            return false;
        }
        const auto parts = split_path(path);
        if (!parts || parts->empty()) {
            return false;
        }
        const auto key = normalize_path(path);
        if (!key) {
            return false;
        }

        std::string parent_norm;
        std::string parent_doc_id;
        std::string parent_orig_rel;
        if (!ensure_dir_chain(*parts, parts->size() - 1, parent_norm, parent_doc_id, parent_orig_rel)) {
            return false;
        }

        const auto existing = index_.find(*key);
        if (existing != index_.end()) {
            if (existing->second.is_directory) {
                return false;
            }
            std::string updated_doc_id;
            if (!backend_->write_document(parent_doc_id, existing->second.document_id,
                                          existing->second.original_name, data, updated_doc_id)) {
                return false;
            }
            if (!updated_doc_id.empty()) {
                existing->second.document_id = std::move(updated_doc_id);
            }
            existing->second.size_bytes = static_cast<std::uint64_t>(data.size());
            return true;
        }

        std::string new_doc_id;
        if (!backend_->write_document(parent_doc_id, {}, parts->back(), data, new_doc_id) ||
            new_doc_id.empty()) {
            return false;
        }

        IndexNode node{};
        node.document_id = std::move(new_doc_id);
        node.parent_document_id = parent_doc_id;
        node.original_name = parts->back();
        node.original_rel_path =
            parent_orig_rel.empty() ? parts->back() : (parent_orig_rel + "/" + parts->back());
        node.is_directory = false;
        node.size_bytes = static_cast<std::uint64_t>(data.size());
        index_.emplace(*key, std::move(node));
        index_[parent_norm].children_keys.push_back(*key);
        ++file_count_;
        return true;
    }

    [[nodiscard]] bool remove_file(std::string_view path) override {
        if (!backend_ || !index_complete_) {
            return false;
        }
        const auto key = normalize_path(path);
        if (!key || key->empty()) {
            return false;
        }
        const auto it = index_.find(*key);
        if (it == index_.end() || it->second.is_directory) {
            return false;
        }
        if (!backend_->delete_document(it->second.document_id)) {
            return false;
        }
        erase_from_parent(*key);
        index_.erase(it);
        if (file_count_ > 0) {
            --file_count_;
        }
        return true;
    }

    // Replaces `to` with `from` through the backend's move / rename (`Vfs::rename_file`
    // contract: `to`'s parent must exist). SAF has no atomic replace, so an existing `to` is
    // deleted first; that window is still far shorter than the remove + write fallback the
    // callers use when this returns false (a backend without rename / move support).
    [[nodiscard]] bool rename_file(std::string_view from, std::string_view to) override {
        if (!backend_ || !index_complete_) {
            return false;
        }
        const auto from_key = normalize_path(from);
        const auto to_key = normalize_path(to);
        const auto to_parts = split_path(to);
        if (!from_key || !to_key || !to_parts || to_parts->empty() || from_key->empty() ||
            to_key->empty()) {
            return false;
        }
        const auto src = index_.find(*from_key);
        if (src == index_.end() || src->second.is_directory) {
            return false;
        }
        if (*from_key == *to_key) {
            return true;
        }
        const std::size_t slash = to_key->rfind('/');
        const std::string to_parent_norm =
            (slash == std::string::npos) ? std::string() : to_key->substr(0, slash);
        const auto parent = index_.find(to_parent_norm);
        if (parent == index_.end() || !parent->second.is_directory) {
            return false;
        }
        const std::string target_parent_doc = parent->second.document_id;
        const std::string target_parent_rel = parent->second.original_rel_path;
        const std::string new_name = to_parts->back();

        if (const auto dst = index_.find(*to_key); dst != index_.end()) {
            if (dst->second.is_directory || !backend_->delete_document(dst->second.document_id)) {
                return false;
            }
            erase_from_parent(*to_key);
            index_.erase(dst);
            if (file_count_ > 0) {
                --file_count_;
            }
        }

        IndexNode node = index_.at(*from_key);
        std::string doc = node.document_id;
        if (node.parent_document_id != target_parent_doc) {
            std::string moved;
            if (!backend_->move_document(doc, node.parent_document_id, target_parent_doc, moved)) {
                return false;
            }
            if (!moved.empty()) {
                doc = std::move(moved);
            }
        }
        if (node.original_name != new_name) {
            std::string renamed;
            if (!backend_->rename_document(doc, new_name, renamed)) {
                if (node.parent_document_id != target_parent_doc) {
                    // Moved but not renamed: re-read the tree so the index still finds it.
                    (void)rebuild_index();
                }
                return false;
            }
            if (!renamed.empty()) {
                doc = std::move(renamed);
            }
        }

        erase_from_parent(*from_key);
        index_.erase(*from_key);
        node.document_id = std::move(doc);
        node.parent_document_id = target_parent_doc;
        node.original_name = new_name;
        node.original_rel_path =
            target_parent_rel.empty() ? new_name : (target_parent_rel + "/" + new_name);
        index_.emplace(*to_key, std::move(node));
        index_[to_parent_norm].children_keys.push_back(*to_key);
        return true;
    }

    [[nodiscard]] bool list_dir(std::string_view path, std::vector<VfsEntry>& out) override {
        out.clear();
        if (!index_complete_) {
            return false;
        }
        const auto key = normalize_path(path);
        if (!key) {
            return false;
        }
        const auto it = index_.find(*key);
        if (it == index_.end() || !it->second.is_directory) {
            return false;
        }
        std::map<std::string, VfsEntry> sorted;
        for (const std::string& child_key : it->second.children_keys) {
            const auto child_it = index_.find(child_key);
            if (child_it == index_.end()) {
                continue;
            }
            sorted.emplace(ascii_lower(child_it->second.original_name),
                           VfsEntry{child_it->second.original_name, child_it->second.is_directory});
        }
        out.reserve(sorted.size());
        for (auto& [k, entry] : sorted) {
            (void)k;
            out.push_back(std::move(entry));
        }
        return true;
    }

    [[nodiscard]] bool is_file(std::string_view path) override {
        if (!index_complete_) {
            return false;
        }
        const auto key = normalize_path(path);
        if (!key || key->empty()) {
            return false;
        }
        const auto it = index_.find(*key);
        return it != index_.end() && !it->second.is_directory;
    }

    [[nodiscard]] bool is_dir(std::string_view path) override {
        if (!index_complete_) {
            return false;
        }
        const auto key = normalize_path(path);
        if (!key) {
            return false;
        }
        const auto it = index_.find(*key);
        return it != index_.end() && it->second.is_directory;
    }

    [[nodiscard]] bool make_dir(std::string_view path) override {
        if (!backend_ || !index_complete_) {
            return false;
        }
        const auto parts = split_path(path);
        if (!parts || parts->empty()) {
            return false;
        }
        const auto key = normalize_path(path);
        if (!key || index_.count(*key) != 0) {
            return false;
        }

        std::string parent_norm;
        std::string parent_doc_id;
        std::string parent_orig_rel;
        if (!ensure_dir_chain(*parts, parts->size() - 1, parent_norm, parent_doc_id, parent_orig_rel)) {
            return false;
        }

        std::string new_doc_id;
        if (!backend_->create_directory(parent_doc_id, parts->back(), new_doc_id) ||
            new_doc_id.empty()) {
            return false;
        }

        IndexNode node{};
        node.document_id = std::move(new_doc_id);
        node.parent_document_id = parent_doc_id;
        node.original_name = parts->back();
        node.original_rel_path =
            parent_orig_rel.empty() ? parts->back() : (parent_orig_rel + "/" + parts->back());
        node.is_directory = true;
        index_.emplace(*key, std::move(node));
        index_[parent_norm].children_keys.push_back(*key);
        ++dir_count_;
        return true;
    }

    [[nodiscard]] bool remove_dir(std::string_view path) override {
        if (!backend_ || !index_complete_) {
            return false;
        }
        const auto key = normalize_path(path);
        if (!key || key->empty()) {
            return false;
        }
        const auto it = index_.find(*key);
        if (it == index_.end() || !it->second.is_directory || !it->second.children_keys.empty()) {
            return false;
        }
        if (!backend_->delete_document(it->second.document_id)) {
            return false;
        }
        erase_from_parent(*key);
        index_.erase(it);
        if (dir_count_ > 0) {
            --dir_count_;
        }
        return true;
    }

    // Copies the indexed files (and directories) matching `sub_tree` (or everything
    // when `sub_tree` is empty) to the same relative paths inside `dest`.
    [[nodiscard]] bool import_to(Vfs& dest,
                                 std::string_view sub_tree = "",
                                 std::size_t* out_files_copied = nullptr,
                                 std::uint64_t* out_bytes_copied = nullptr) {
        if (out_files_copied != nullptr) {
            *out_files_copied = 0;
        }
        if (out_bytes_copied != nullptr) {
            *out_bytes_copied = 0;
        }
        if (!backend_ || !index_complete_) {
            return false;
        }
        const auto root_key = normalize_path(sub_tree);
        if (!root_key) {
            return false;
        }
        if (!root_key->empty() && index_.count(*root_key) == 0) {
            return false;
        }

        const std::string prefix = root_key->empty() ? std::string() : (*root_key + "/");
        std::vector<const IndexNode*> dirs;
        std::vector<const IndexNode*> files;
        for (const auto& [key, node] : index_) {
            if (key.empty()) {
                continue;
            }
            const bool match = root_key->empty() || key == *root_key ||
                               (key.size() > prefix.size() &&
                                key.compare(0, prefix.size(), prefix) == 0);
            if (!match) {
                continue;
            }
            if (node.is_directory) {
                dirs.push_back(&node);
            } else {
                files.push_back(&node);
            }
        }

        auto by_rel_path = [](const IndexNode* a, const IndexNode* b) {
            return ascii_lower(a->original_rel_path) < ascii_lower(b->original_rel_path);
        };
        std::sort(dirs.begin(), dirs.end(), by_rel_path);
        std::sort(files.begin(), files.end(), by_rel_path);

        for (const IndexNode* dir : dirs) {
            if (!ensure_dest_dirs(dest, dir->original_rel_path)) {
                return false;
            }
        }

        std::size_t copied_files = 0;
        std::uint64_t copied_bytes = 0;
        std::vector<std::uint8_t> bytes;
        for (const IndexNode* file : files) {
            const std::size_t slash = file->original_rel_path.rfind('/');
            if (slash != std::string::npos) {
                if (!ensure_dest_dirs(dest, std::string_view(file->original_rel_path).substr(0, slash))) {
                    return false;
                }
            }
            if (!backend_->read_document(file->document_id, bytes)) {
                return false;
            }
            if (!dest.write_file(file->original_rel_path, bytes)) {
                return false;
            }
            ++copied_files;
            copied_bytes += static_cast<std::uint64_t>(bytes.size());
        }

        if (out_files_copied != nullptr) {
            *out_files_copied = copied_files;
        }
        if (out_bytes_copied != nullptr) {
            *out_bytes_copied = copied_bytes;
        }
        return true;
    }

private:
    struct IndexNode {
        std::string document_id{};
        std::string parent_document_id{};
        std::string original_name{};
        std::string original_rel_path{};
        bool is_directory = false;
        std::uint64_t size_bytes = 0;
        std::vector<std::string> children_keys{};
    };

    static bool is_safe_child_name(std::string_view name) noexcept {
        return !name.empty() && name != "." && name != ".." &&
               name.find('/') == std::string_view::npos &&
               name.find('\\') == std::string_view::npos &&
               name.find(':') == std::string_view::npos &&
               name.find('\0') == std::string_view::npos;
    }

    static bool ensure_dest_dirs(Vfs& dest, std::string_view rel_dir) {
        const auto parts = split_path(rel_dir);
        if (!parts) {
            return false;
        }
        std::string cur;
        for (const std::string& part : *parts) {
            if (!cur.empty()) {
                cur.push_back('/');
            }
            cur.append(part);
            if (dest.is_dir(cur)) {
                continue;
            }
            if (dest.is_file(cur) || !dest.make_dir(cur)) {
                return false;
            }
        }
        return true;
    }

    void erase_from_parent(std::string_view child_key) {
        const std::size_t slash = child_key.rfind('/');
        const std::string parent_key =
            (slash == std::string_view::npos) ? std::string() : std::string(child_key.substr(0, slash));
        const auto pit = index_.find(parent_key);
        if (pit == index_.end()) {
            return;
        }
        auto& kids = pit->second.children_keys;
        kids.erase(std::remove(kids.begin(), kids.end(), child_key), kids.end());
    }

    bool ensure_dir_chain(const std::vector<std::string>& parts,
                          std::size_t count,
                          std::string& out_parent_norm,
                          std::string& out_parent_doc_id,
                          std::string& out_parent_orig_rel) {
        const auto root_it = index_.find("");
        if (root_it == index_.end() || !root_it->second.is_directory) {
            return false;
        }
        std::string norm;
        std::string doc_id = root_it->second.document_id;
        std::string orig_rel;

        for (std::size_t i = 0; i < count; ++i) {
            const std::string& comp = parts[i];
            const std::string next_norm = norm.empty() ? ascii_lower(comp) : (norm + "/" + ascii_lower(comp));
            const std::string next_orig_rel = orig_rel.empty() ? comp : (orig_rel + "/" + comp);
            const auto it = index_.find(next_norm);
            if (it != index_.end()) {
                if (!it->second.is_directory) {
                    return false;
                }
                norm = next_norm;
                doc_id = it->second.document_id;
                orig_rel = it->second.original_rel_path;
            } else {
                std::string new_doc_id;
                if (!backend_->create_directory(doc_id, comp, new_doc_id) || new_doc_id.empty()) {
                    return false;
                }
                IndexNode node{};
                node.document_id = new_doc_id;
                node.parent_document_id = doc_id;
                node.original_name = comp;
                node.original_rel_path = next_orig_rel;
                node.is_directory = true;
                index_.emplace(next_norm, std::move(node));
                index_[norm].children_keys.push_back(next_norm);
                ++dir_count_;
                norm = next_norm;
                doc_id = std::move(new_doc_id);
                orig_rel = next_orig_rel;
            }
        }

        out_parent_norm = std::move(norm);
        out_parent_doc_id = std::move(doc_id);
        out_parent_orig_rel = std::move(orig_rel);
        return true;
    }

    std::shared_ptr<SafTreeBackend> backend_{};
    std::unordered_map<std::string, IndexNode> index_{};
    bool index_complete_ = false;
    std::size_t file_count_ = 0;
    std::size_t dir_count_ = 0;
};

struct WarbandModuleInfo {
    std::string folder_name{};
    std::string module_path{};
    std::string display_name{};
    bool has_module_ini = false;
    bool has_scripts_txt = false;
    std::size_t load_resource_count = 0;
    std::size_t load_mod_resource_count = 0;
    std::string compatible_with_warband{};
};

// Discovers Warband modules under `Modules/*/module.ini` using any `Vfs`.
// Sorts `Native` first, followed by remaining modules sorted case-insensitively.
[[nodiscard]] inline std::vector<WarbandModuleInfo> scan_warband_modules(
    Vfs& warband_vfs,
    std::string_view modules_root = "Modules") {
    std::vector<VfsEntry> dirs;
    if (!warband_vfs.list_dir(modules_root, dirs)) {
        return {};
    }

    auto find_setting_ci = [](const std::unordered_map<std::string, std::string>& settings,
                              std::string_view key_lower) -> std::string {
        const auto exact = settings.find(std::string(key_lower));
        if (exact != settings.end()) {
            return exact->second;
        }
        for (const auto& [k, v] : settings) {
            if (ascii_lower(k) == key_lower) {
                return v;
            }
        }
        return {};
    };

    std::vector<WarbandModuleInfo> modules;
    std::string ini_text;
    for (const VfsEntry& entry : dirs) {
        if (!entry.is_directory) {
            continue;
        }
        const std::string mod_path = game::vfs_join_path(modules_root, entry.name);
        const std::string ini_path = game::vfs_join_path(mod_path, "module.ini");
        if (!warband_vfs.is_file(ini_path) || !warband_vfs.read_text(ini_path, ini_text)) {
            continue;
        }

        game::ModuleIniData ini{};
        game::parse_module_ini(ini_text, ini);

        WarbandModuleInfo info{};
        info.folder_name = entry.name;
        info.module_path = mod_path;
        info.has_module_ini = true;
        info.has_scripts_txt = warband_vfs.is_file(game::vfs_join_path(mod_path, "scripts.txt"));
        info.load_resource_count = ini.load_resources.size();
        info.load_mod_resource_count = ini.load_mod_resources.size();

        std::string name = find_setting_ci(ini.settings, "module_name");
        if (!name.empty()) {
            game::underscores_to_spaces(name);
            info.display_name = std::move(name);
        } else {
            info.display_name = entry.name;
        }
        info.compatible_with_warband = find_setting_ci(ini.settings, "compatible_with_warband");
        modules.push_back(std::move(info));
    }

    std::sort(modules.begin(), modules.end(), [](const WarbandModuleInfo& a, const WarbandModuleInfo& b) {
        const std::string la = ascii_lower(a.folder_name);
        const std::string lb = ascii_lower(b.folder_name);
        const bool a_native = (la == "native");
        const bool b_native = (lb == "native");
        if (a_native != b_native) {
            return a_native;
        }
        if (la != lb) {
            return la < lb;
        }
        return a.folder_name < b.folder_name;
    });

    return modules;
}

struct ModuleScriptsCheckResult {
    bool ok = false;
    std::size_t script_count = 0;
    std::size_t statement_count = 0;
    std::size_t mapped_script_count = 0;
    std::string error_message{};
};

// Reads and validates `<module_rel_dir>/scripts.txt` through `Vfs`.
[[nodiscard]] inline ModuleScriptsCheckResult verify_module_scripts(
    Vfs& warband_vfs,
    std::string_view module_rel_dir) {
    ModuleScriptsCheckResult result{};
    const std::string path = game::vfs_join_path(module_rel_dir, "scripts.txt");
    std::string text;
    if (!warband_vfs.read_text(path, text)) {
        result.error_message = "unable to read " + path;
        return result;
    }

    io::TokenReader reader(text);
    game::ScriptManager mgr{};
    const vm::ReadError err = game::read_script_manager(reader, mgr);
    if (err != vm::ReadError::none) {
        result.error_message = std::string(vm::describe(err));
        return result;
    }

    std::size_t total_stmts = 0;
    for (const game::Script& script : mgr.scripts) {
        total_stmts += script.block.statements.size();
    }
    std::size_t mapped = 0;
    for (const std::int32_t idx : mgr.mappedScripts) {
        if (idx >= 0) {
            ++mapped;
        }
    }

    result.ok = true;
    result.script_count = mgr.scripts.size();
    result.statement_count = total_stmts;
    result.mapped_script_count = mapped;
    return result;
}

}  // namespace wse::platform
