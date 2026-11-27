// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "common/settings.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/process_creation.h"
#include "core/hle/service/am/service/application_accessor.h"
#include "core/hle/service/am/service/application_creator.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/loader/loader.h"
#include "core/launch_timestamp_cache.h"
#include "core/hle/api_version.h"

namespace Service::AM {

namespace {

constexpr u32 LaunchParameterAccountPreselectedUserMagic = 0xC79497CA;

struct LaunchParameterAccountPreselectedUser {
    u32 magic;
    u32 is_account_selected;
    Common::UUID current_user;
    INSERT_PADDING_BYTES(0x70);
};
static_assert(sizeof(LaunchParameterAccountPreselectedUser) == 0x88);

Result CreateGuestApplication(SharedPointer<IApplicationAccessor>* out_application_accessor, Core::System& system, WindowSystem& window_system, u64 program_id) {
    FileSys::VirtualFile nca_raw{};

    // Get the program NCA from storage.
    auto& storage = system.GetContentProviderUnion();
    nca_raw = storage.GetEntryRaw(program_id, FileSys::ContentRecordType::Program);

    // Ensure we retrieved a program NCA.
    R_UNLESS(nca_raw != nullptr, ResultUnknown);

    std::vector<u8> control;
    std::unique_ptr<Loader::AppLoader> loader;
    Loader::ResultStatus result;
    auto process = CreateApplicationProcess(control, loader, result, system, nca_raw, program_id, 0);
    R_UNLESS(process != nullptr, ResultUnknown);

    const auto applet = std::make_shared<Applet>(system, std::move(process), true);
    applet->program_id = program_id;
    applet->applet_id = AppletId::Application;
    applet->type = AppletType::Application;
    applet->library_applet_mode = LibraryAppletMode::AllForeground;

    // Applications started from the Home Menu do not get the preselected user that frontend
    // launches push in AppletManager. Games that require a startup account then pop an empty
    // launch parameter, query a zero user id and abort. Preselect the configured user here too.
    {
        Account::ProfileManager profile_manager{};
        if (const auto uuid =
                profile_manager.GetUser(static_cast<s32>(Settings::values.current_user.GetValue()));
            uuid.has_value() && uuid->IsValid()) {
            LaunchParameterAccountPreselectedUser lp{};
            lp.magic = LaunchParameterAccountPreselectedUserMagic;
            lp.is_account_selected = 1;
            lp.current_user = *uuid;

            std::vector<u8> buffer(sizeof(LaunchParameterAccountPreselectedUser));
            std::memcpy(buffer.data(), &lp, buffer.size());
            applet->preselected_user_launch_parameter.push_back(std::move(buffer));
        }
    }

    window_system.TrackApplet(applet, true);

    *out_application_accessor = std::make_shared<IApplicationAccessor>(system, applet, window_system);
    R_SUCCEED();
}

} // namespace

IApplicationCreator::IApplicationCreator(Core::System& system_, WindowSystem& window_system)
    : ServiceFramework{system_, "IApplicationCreator"}, m_window_system{window_system} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IApplicationCreator::CreateApplication>, "CreateApplication"},
        {1, nullptr, "PopLaunchRequestedApplication"},
        {10, D<&IApplicationCreator::CreateSystemApplication>, "CreateSystemApplication"},
        {100, nullptr, "PopFloatingApplicationForDevelopment"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IApplicationCreator::~IApplicationCreator() = default;

Result IApplicationCreator::CreateApplication(
    Out<SharedPointer<IApplicationAccessor>> out_application_accessor, u64 application_id) {
    LOG_INFO(Service_NS, "called, application_id={:016X}", application_id);
    Core::LaunchTimestampCache::SaveLaunchTimestamp(application_id);
    R_RETURN(
        CreateGuestApplication(out_application_accessor, system, m_window_system, application_id));
}

Result IApplicationCreator::CreateSystemApplication(
    Out<SharedPointer<IApplicationAccessor>> out_application_accessor, u64 application_id) {

    FileSys::VirtualFile nca_raw{};

    auto& storage = system.GetContentProviderUnion();
    nca_raw = storage.GetEntryRaw(application_id, FileSys::ContentRecordType::Program);

    R_UNLESS(nca_raw != nullptr, ResultUnknown);

    std::vector<u8> control;
    std::unique_ptr<Loader::AppLoader> loader;
    auto process = CreateProcess(system, application_id, 1, HLE::ApiVersion::HOS_VERSION_MAJOR);
    R_UNLESS(process != nullptr, ResultUnknown);

    const auto applet = std::make_shared<Applet>(system, std::move(process), true);
    applet->program_id = application_id;
    applet->applet_id = AppletId::Starter;
    applet->type = AppletType::LibraryApplet;
    applet->library_applet_mode = LibraryAppletMode::AllForeground;

    m_window_system.TrackApplet(applet, true);

    *out_application_accessor = std::make_shared<IApplicationAccessor>(system, applet, m_window_system);
    Core::LaunchTimestampCache::SaveLaunchTimestamp(application_id);
    R_SUCCEED();
}

} // namespace Service::AM
