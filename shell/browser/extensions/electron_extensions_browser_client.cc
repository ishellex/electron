// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/browser/extensions/electron_extensions_browser_client.h"

#include <utility>

#include "base/bind.h"
#include "base/memory/ptr_util.h"
#include "base/path_service.h"
#include "base/task/post_task.h"
#include "build/build_config.h"
#include "chrome/browser/extensions/chrome_url_request_util.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/extensions/chrome_manifest_url_handlers.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/common/user_agent.h"
#include "extensions/browser/api/extensions_api_client.h"
#include "extensions/browser/component_extension_resource_manager.h"
#include "extensions/browser/core_extensions_browser_api_provider.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/extension_protocols.h"
#include "extensions/browser/extensions_browser_interface_binders.h"
#include "extensions/browser/null_app_sorting.h"
#include "extensions/browser/updater/null_extension_cache.h"
#include "extensions/browser/url_request_util.h"
#include "extensions/common/features/feature_channel.h"
#include "extensions/common/file_util.h"
#include "extensions/common/manifest_constants.h"
#include "extensions/common/manifest_url_handlers.h"
#include "net/base/mime_util.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "shell/browser/api/electron_api_session.h"
#include "shell/browser/api/electron_api_web_contents.h"
#include "shell/browser/browser.h"
#include "shell/browser/electron_browser_client.h"
#include "shell/browser/electron_browser_context.h"
#include "shell/browser/extensions/api/runtime/electron_runtime_api_delegate.h"
#include "shell/browser/extensions/electron_component_extension_resource_manager.h"
#include "shell/browser/extensions/electron_extension_host_delegate.h"
#include "shell/browser/extensions/electron_extension_system_factory.h"
#include "shell/browser/extensions/electron_extension_web_contents_observer.h"
#include "shell/browser/extensions/electron_extensions_api_client.h"
#include "shell/browser/extensions/electron_extensions_browser_api_provider.h"
#include "shell/browser/extensions/electron_kiosk_delegate.h"
#include "shell/browser/extensions/electron_navigation_ui_data.h"
#include "shell/browser/extensions/electron_process_manager_delegate.h"
#include "ui/base/resource/resource_bundle.h"

using content::BrowserContext;
using content::BrowserThread;
using extensions::ExtensionsBrowserClient;

namespace electron {

ElectronExtensionsBrowserClient::ElectronExtensionsBrowserClient()
    : api_client_(new extensions::ElectronExtensionsAPIClient),
      process_manager_delegate_(new extensions::ElectronProcessManagerDelegate),
      extension_cache_(new extensions::NullExtensionCache()) {
  // Electron does not have a concept of channel, so leave UNKNOWN to
  // enable all channel-dependent extension APIs.
  extensions::SetCurrentChannel(version_info::Channel::UNKNOWN);
  resource_manager_.reset(
      new extensions::ElectronComponentExtensionResourceManager());

  AddAPIProvider(
      std::make_unique<extensions::CoreExtensionsBrowserAPIProvider>());
  AddAPIProvider(
      std::make_unique<extensions::ElectronExtensionsBrowserAPIProvider>());
}

ElectronExtensionsBrowserClient::~ElectronExtensionsBrowserClient() {}

bool ElectronExtensionsBrowserClient::IsShuttingDown() {
  return electron::Browser::Get()->is_shutting_down();
}

bool ElectronExtensionsBrowserClient::AreExtensionsDisabled(
    const base::CommandLine& command_line,
    BrowserContext* context) {
  return false;
}

bool ElectronExtensionsBrowserClient::IsValidContext(BrowserContext* context) {
  auto& context_map = ElectronBrowserContext::browser_context_map();
  for (auto const& entry : context_map) {
    if (entry.second && entry.second.get() == context)
      return true;
  }
  return false;
}

bool ElectronExtensionsBrowserClient::IsSameContext(BrowserContext* first,
                                                    BrowserContext* second) {
  return first == second;
}

bool ElectronExtensionsBrowserClient::HasOffTheRecordContext(
    BrowserContext* context) {
  return false;
}

BrowserContext* ElectronExtensionsBrowserClient::GetOffTheRecordContext(
    BrowserContext* context) {
  // app_shell only supports a single context.
  return nullptr;
}

BrowserContext* ElectronExtensionsBrowserClient::GetOriginalContext(
    BrowserContext* context) {
  DCHECK(context);
  if (context->IsOffTheRecord()) {
    return ElectronBrowserContext::From("", false);
  } else {
    return context;
  }
}

bool ElectronExtensionsBrowserClient::IsGuestSession(
    BrowserContext* context) const {
  return false;
}

bool ElectronExtensionsBrowserClient::IsExtensionIncognitoEnabled(
    const std::string& extension_id,
    content::BrowserContext* context) const {
  return false;
}

bool ElectronExtensionsBrowserClient::CanExtensionCrossIncognito(
    const extensions::Extension* extension,
    content::BrowserContext* context) const {
  return false;
}

base::FilePath ElectronExtensionsBrowserClient::GetBundleResourcePath(
    const network::ResourceRequest& request,
    const base::FilePath& extension_resources_path,
    int* resource_id) const {
  *resource_id = 0;
  base::FilePath chrome_resources_path;
  if (!base::PathService::Get(chrome::DIR_RESOURCES, &chrome_resources_path))
    return base::FilePath();

  // Since component extension resources are included in
  // component_extension_resources.pak file in |chrome_resources_path|,
  // calculate the extension |request_relative_path| against
  // |chrome_resources_path|.
  if (!chrome_resources_path.IsParent(extension_resources_path))
    return base::FilePath();

  const base::FilePath request_relative_path =
      extensions::file_util::ExtensionURLToRelativeFilePath(request.url);
  if (!ExtensionsBrowserClient::Get()
           ->GetComponentExtensionResourceManager()
           ->IsComponentExtensionResource(extension_resources_path,
                                          request_relative_path, resource_id)) {
    return base::FilePath();
  }
  DCHECK_NE(0, *resource_id);

  return request_relative_path;
}

void ElectronExtensionsBrowserClient::LoadResourceFromResourceBundle(
    const network::ResourceRequest& request,
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    const base::FilePath& resource_relative_path,
    int resource_id,
    const std::string& content_security_policy,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    bool send_cors_header) {
  extensions::chrome_url_request_util::LoadResourceFromResourceBundle(
      request, std::move(loader), resource_relative_path, resource_id,
      content_security_policy, std::move(client), send_cors_header);
}

namespace {
bool AllowCrossRendererResourceLoad(
    const network::ResourceRequest& request,
    network::mojom::RequestDestination destination,
    ui::PageTransition page_transition,
    int child_id,
    bool is_incognito,
    const extensions::Extension* extension,
    const extensions::ExtensionSet& extensions,
    const extensions::ProcessMap& process_map,
    bool* allowed) {
  if (extensions::url_request_util::AllowCrossRendererResourceLoad(
          request, destination, page_transition, child_id, is_incognito,
          extension, extensions, process_map, allowed)) {
    return true;
  }

  // If there aren't any explicitly marked web accessible resources, the
  // load should be allowed only if it is by DevTools. A close approximation is
  // checking if the extension contains a DevTools page.
  if (extension && !extensions::chrome_manifest_urls::GetDevToolsPage(extension)
                        .is_empty()) {
    *allowed = true;
    return true;
  }

  // Couldn't determine if the resource is allowed or not.
  return false;
}
}  // namespace

bool ElectronExtensionsBrowserClient::AllowCrossRendererResourceLoad(
    const network::ResourceRequest& request,
    network::mojom::RequestDestination destination,
    ui::PageTransition page_transition,
    int child_id,
    bool is_incognito,
    const extensions::Extension* extension,
    const extensions::ExtensionSet& extensions,
    const extensions::ProcessMap& process_map) {
  bool allowed = false;
  if (::electron::AllowCrossRendererResourceLoad(
          request, destination, page_transition, child_id, is_incognito,
          extension, extensions, process_map, &allowed)) {
    return allowed;
  }

  // Couldn't determine if resource is allowed. Block the load.
  return false;
}

PrefService* ElectronExtensionsBrowserClient::GetPrefServiceForContext(
    BrowserContext* context) {
  return static_cast<ElectronBrowserContext*>(context)->prefs();
}

void ElectronExtensionsBrowserClient::GetEarlyExtensionPrefsObservers(
    content::BrowserContext* context,
    std::vector<extensions::EarlyExtensionPrefsObserver*>* observers) const {}

extensions::ProcessManagerDelegate*
ElectronExtensionsBrowserClient::GetProcessManagerDelegate() const {
  return process_manager_delegate_.get();
}

std::unique_ptr<extensions::ExtensionHostDelegate>
ElectronExtensionsBrowserClient::
    CreateExtensionHostDelegate() {  // TODO(samuelmaddock):
  return base::WrapUnique(new extensions::ElectronExtensionHostDelegate);
}

bool ElectronExtensionsBrowserClient::DidVersionUpdate(
    BrowserContext* context) {
  // TODO(jamescook): We might want to tell extensions when app_shell updates.
  return false;
}

void ElectronExtensionsBrowserClient::PermitExternalProtocolHandler() {}

bool ElectronExtensionsBrowserClient::IsInDemoMode() {
  return false;
}

bool ElectronExtensionsBrowserClient::IsScreensaverInDemoMode(
    const std::string& app_id) {
  return false;
}

bool ElectronExtensionsBrowserClient::IsRunningInForcedAppMode() {
  return false;
}

bool ElectronExtensionsBrowserClient::IsAppModeForcedForApp(
    const extensions::ExtensionId& extension_id) {
  return false;
}

bool ElectronExtensionsBrowserClient::IsLoggedInAsPublicAccount() {
  return false;
}

extensions::ExtensionSystemProvider*
ElectronExtensionsBrowserClient::GetExtensionSystemFactory() {
  return extensions::ElectronExtensionSystemFactory::GetInstance();
}

std::unique_ptr<extensions::RuntimeAPIDelegate>
ElectronExtensionsBrowserClient::CreateRuntimeAPIDelegate(
    content::BrowserContext* context) const {
  return std::make_unique<extensions::ElectronRuntimeAPIDelegate>(context);
}

const extensions::ComponentExtensionResourceManager*
ElectronExtensionsBrowserClient::GetComponentExtensionResourceManager() {
  return resource_manager_.get();
}

void ElectronExtensionsBrowserClient::BroadcastEventToRenderers(
    extensions::events::HistogramValue histogram_value,
    const std::string& event_name,
    std::unique_ptr<base::ListValue> args,
    bool dispatch_to_off_the_record_profiles) {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    base::PostTask(
        FROM_HERE, {BrowserThread::UI},
        base::BindOnce(
            &ElectronExtensionsBrowserClient::BroadcastEventToRenderers,
            base::Unretained(this), histogram_value, event_name,
            std::move(args), dispatch_to_off_the_record_profiles));
    return;
  }

  std::unique_ptr<extensions::Event> event(
      new extensions::Event(histogram_value, event_name, std::move(args)));
  auto& context_map = ElectronBrowserContext::browser_context_map();
  for (auto const& entry : context_map) {
    if (entry.second) {
      extensions::EventRouter::Get(entry.second.get())
          ->BroadcastEvent(std::move(event));
    }
  }
}

extensions::ExtensionCache*
ElectronExtensionsBrowserClient::GetExtensionCache() {
  return extension_cache_.get();
}

bool ElectronExtensionsBrowserClient::IsBackgroundUpdateAllowed() {
  return true;
}

bool ElectronExtensionsBrowserClient::IsMinBrowserVersionSupported(
    const std::string& min_version) {
  return true;
}

void ElectronExtensionsBrowserClient::SetAPIClientForTest(
    extensions::ExtensionsAPIClient* api_client) {
  api_client_.reset(api_client);
}

extensions::ExtensionWebContentsObserver*
ElectronExtensionsBrowserClient::GetExtensionWebContentsObserver(
    content::WebContents* web_contents) {
  return extensions::ElectronExtensionWebContentsObserver::FromWebContents(
      web_contents);
}

extensions::KioskDelegate* ElectronExtensionsBrowserClient::GetKioskDelegate() {
  if (!kiosk_delegate_)
    kiosk_delegate_.reset(new ElectronKioskDelegate());
  return kiosk_delegate_.get();
}

bool ElectronExtensionsBrowserClient::IsLockScreenContext(
    content::BrowserContext* context) {
  return false;
}

std::string ElectronExtensionsBrowserClient::GetApplicationLocale() {
  return ElectronBrowserClient::Get()->GetApplicationLocale();
}

std::string ElectronExtensionsBrowserClient::GetUserAgent() const {
  return ElectronBrowserClient::Get()->GetUserAgent();
}

void ElectronExtensionsBrowserClient::RegisterBrowserInterfaceBindersForFrame(
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map,
    content::RenderFrameHost* render_frame_host,
    const extensions::Extension* extension) const {
  PopulateExtensionFrameBinders(map, render_frame_host, extension);
}

void ElectronExtensionsBrowserClient::GetTabAndWindowIdForWebContents(
    content::WebContents* web_contents,
    int* tab_id,
    int* window_id) {
  api::WebContents* api_wc = api::WebContents::From(web_contents);
  auto* session =
      api::Session::FromBrowserContext(web_contents->GetBrowserContext());
  auto tab = api_wc ? session->GetChromeTabDetails(api_wc) : nullptr;

  *tab_id = tab ? tab->id : -1;
  *window_id = tab ? tab->window_id : -1;
}

}  // namespace electron
