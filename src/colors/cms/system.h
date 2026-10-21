// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Access operating system wide information about color management.
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2017 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#ifndef SEEN_COLORS_CMS_SYSTEM_H
#define SEEN_COLORS_CMS_SYSTEM_H

#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "colors/spaces/enum.h"
#include "preferences.h"

using DirPaths = std::vector<std::pair<std::string, bool>>;

namespace Inkscape::Colors::CMS {

class Profile;
class TransformSurface;

/**
 * The soft-proofing preferences from /options/softproof/*.
 *
 * gamut_color is packed as 0xRRGGBBAA, matching the "#rrggbb" string the
 * preferences UI stores.
 */
struct ProofSettings
{
    bool enabled = false;
    bool gamut_warn = false;
    uint32_t gamut_color = 0x808080ff;
    RenderingIntent intent = RenderingIntent::PERCEPTUAL;
    bool bpc = false;
    std::string uri;

    bool operator==(ProofSettings const &other) const = default;
};

class System
{
private:
    System(System const &) = delete;
    void operator=(System const &) = delete;

public:
    // Access the singleton CMS::System object.
    static System &get();

    DirPaths const &getDirectoryPaths();
    void addDirectoryPath(std::string path, bool is_user);
    void clearDirectoryPaths();

    std::vector<std::shared_ptr<Profile>> getProfiles() const;
    const std::shared_ptr<Profile> &getProfile(std::string const &name) const;

    std::vector<std::shared_ptr<Profile>> getDisplayProfiles() const;
    const std::shared_ptr<Profile> &getDisplayProfile(bool &updated);
    const std::shared_ptr<TransformSurface> &getDisplayTransform();

    std::vector<std::shared_ptr<Profile>> getOutputProfiles() const;

    static ProofSettings readProofSettings();
    static std::shared_ptr<Profile>
    selectProofProfile(ProofSettings const &settings, std::shared_ptr<Profile> const &document_output,
                       std::function<std::shared_ptr<Profile>(std::string const &)> const &by_uri);
    std::shared_ptr<TransformSurface> getProofTransform(std::shared_ptr<Profile> const &document_output);

    void refreshProfiles();

    System();
    ~System() = default;

    // Used by testing to add profiles when needed
    void addProfile(std::shared_ptr<Profile> profile) { _profiles.emplace_back(profile); }

private:
    // List of ICC profiles found on system
    std::vector<std::shared_ptr<Profile>> _profiles;

    // Paths to search for icc profiles
    DirPaths _paths;

    // We track last display transform settings. If there is a change, we delete create new transform.
    std::shared_ptr<Profile> _display_profile;
    std::shared_ptr<TransformSurface> _display_transform;
    bool _display = false;
    int _display_intent = -1;

    struct ProofCacheKey
    {
        ProofSettings settings;
        std::string proof_id;
        bool display_enabled = false;
        int display_intent = 0;
        std::string display_uri;

        bool operator==(ProofCacheKey const &other) const = default;
    };

    // Soft-proofing transform cache, invalidated whenever any input changes.
    std::optional<ProofCacheKey> _proof_key;
    std::shared_ptr<TransformSurface> _proof_transform;

    Inkscape::PrefObserver _prefs_observer;
};

} // namespace Inkscape::Colors::CMS

#endif // SEEN_COLORS_CMS_SYSTEM_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
