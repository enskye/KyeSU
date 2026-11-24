package me.weishu.kernelsu.ui.viewmodel

import android.content.pm.ApplicationInfo
import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import me.weishu.kernelsu.Natives
import me.weishu.kernelsu.data.repository.SettingsRepository
import me.weishu.kernelsu.data.repository.SettingsRepositoryImpl
import me.weishu.kernelsu.data.repository.SuperUserRepository
import me.weishu.kernelsu.data.repository.SuperUserRepositoryImpl
import me.weishu.kernelsu.ksuApp
import me.weishu.kernelsu.ui.component.SearchStatus
import me.weishu.kernelsu.ui.screen.superuser.GroupedApps
import me.weishu.kernelsu.ui.screen.superuser.SuperUserUiState
import me.weishu.kernelsu.ui.util.PackageEvents
import me.weishu.kernelsu.ui.util.PinyinUtil
import me.weishu.kernelsu.ui.util.ownerNameForUid
import me.weishu.kernelsu.ui.util.pickPrimary
import java.text.Collator
import java.util.Collections
import java.util.IdentityHashMap
import java.util.Locale

internal const val RECENTLY_INSTALLED_WINDOW_MILLIS = 60 * 60 * 1000L

enum class AppSortType {
    NAME, PACKAGE_NAME, INSTALL_TIME, UPDATE_TIME;

    companion object {
        fun fromOrdinal(ordinal: Int): AppSortType =
            entries.getOrElse(ordinal) { NAME }
    }
}

data class AppSortConfig(
    val sortType: AppSortType = AppSortType.NAME,
    val reversed: Boolean = false,
) {
    fun toInt(): Int = sortType.ordinal * 2 + if (reversed) 1 else 0

    fun withType(type: AppSortType): AppSortConfig = copy(sortType = type)
    fun toggleReversed(): AppSortConfig = copy(reversed = !reversed)

    companion object {
        fun fromInt(value: Int): AppSortConfig = AppSortConfig(
            sortType = AppSortType.fromOrdinal(value / 2),
            reversed = value % 2 != 0,
        )
    }
}

internal fun buildRecentlyInstalledGroups(
    groups: List<GroupedApps>,
    nowMillis: Long = System.currentTimeMillis(),
): List<GroupedApps> {
    val cutoffMillis = nowMillis - RECENTLY_INSTALLED_WINDOW_MILLIS
    val collator = Collator.getInstance(Locale.getDefault())

    return groups.mapNotNull { group ->
        val latestInstallTime = group.apps.maxOfOrNull { it.packageInfo.firstInstallTime } ?: return@mapNotNull null
        if (latestInstallTime < cutoffMillis) {
            null
        } else {
            group to latestInstallTime
        }
    }.sortedWith(
        compareByDescending<Pair<GroupedApps, Long>> { it.second }
            .thenBy(collator) { it.first.primary.label }
    ).map { it.first }
}

class SuperUserViewModel(
    private val repo: SuperUserRepository = SuperUserRepositoryImpl(),
    private val settingsRepo: SettingsRepository = SettingsRepositoryImpl()
) : ViewModel() {

    companion object {
        private const val TAG = "SuperUserViewModel"

        private val appsLock = Any()
        private var cachedApps: List<AppInfo> = emptyList()
        private var cachedUserIds: List<Int> = emptyList()
        private data class AppListSnapshot(
            val apps: List<AppInfo>,
            val userIds: List<Int>,
            val packageSeq: Int,
        )

        private var cachedPackageSeq: Int? = null
        private var appListScan: Deferred<Result<AppListSnapshot>>? = null
        private val scanScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
        private val groupedAppsLock = Any()
        private var cachedGroupedApps: List<GroupedApps> = emptyList()

        val apps: List<AppInfo>
            get() = synchronized(appsLock) { cachedApps }

        // One shared scan owns the cache. Events during a scan are coalesced
        // into a follow-up pass; no second repository scan can overlap it.
        private suspend fun scanApps(repo: SuperUserRepository): Result<AppListSnapshot> {
            while (true) {
                val packageSeq = PackageEvents.sequence.value
                val (apps, ids) = repo.getAppList().getOrElse {
                    Log.e(TAG, "loadApps failed", it)
                    return Result.failure(it)
                }
                val snapshot = AppListSnapshot(apps, ids, packageSeq)
                val published = synchronized(appsLock) {
                    if (PackageEvents.sequence.value != packageSeq) {
                        false
                    } else {
                        cachedApps = apps
                        cachedUserIds = ids
                        cachedPackageSeq = packageSeq
                        true
                    }
                }
                if (published) return Result.success(snapshot)
            }
        }

        private suspend fun loadAppSnapshot(
            useCache: Boolean,
            repo: SuperUserRepository,
        ): Result<AppListSnapshot> {
            val scan = synchronized(appsLock) {
                val packageSeq = PackageEvents.sequence.value
                if (useCache && cachedPackageSeq == packageSeq) {
                    return Result.success(AppListSnapshot(cachedApps, cachedUserIds, packageSeq))
                }
                appListScan?.takeUnless { it.isCompleted } ?: scanScope.async {
                    scanApps(repo)
                }.also { appListScan = it }
            }
            return scan.await()
        }

        suspend fun loadApps(
            useCache: Boolean,
            repo: SuperUserRepository = SuperUserRepositoryImpl(),
        ): Result<Pair<List<AppInfo>, List<Int>>> =
            loadAppSnapshot(useCache, repo).map { it.apps to it.userIds }

        @JvmStatic
        fun getGroupedApp(uid: Int): GroupedApps? {
            return synchronized(groupedAppsLock) { cachedGroupedApps.find { it.uid == uid } }
        }
    }

    typealias AppInfo = me.weishu.kernelsu.data.model.AppInfo

    private val _uiState = MutableStateFlow(SuperUserUiState())
    val uiState: StateFlow<SuperUserUiState> = _uiState.asStateFlow()

    private val refreshMutex = Mutex()
    private val searchQuery = MutableStateFlow("")
    private var sortJob: Job? = null
    private var packageWatchJob: Job? = null
    private var scannedPackageSeq: Int? = null
    var isNeedRefresh = false
        private set

    init {
        viewModelScope.launchSearchQueryCollector(searchQuery, ::applySearchText)
    }

    fun markNeedRefresh() {
        isNeedRefresh = true
    }

    fun startPackageWatch() {
        if (packageWatchJob?.isActive == true) return
        PackageEvents.start()
        packageWatchJob = viewModelScope.launch {
            // StateFlow conflates bursts while the current refresh finishes.
            // Observe initial load completion too, so events during it are not lost.
            combine(
                PackageEvents.sequence,
                uiState.map { it.hasLoaded }.distinctUntilChanged(),
            ) { seq, loaded -> seq to loaded }.collect { (seq, loaded) ->
                if (loaded && seq != scannedPackageSeq) fetchAppList(silent = true)
            }
        }
    }

    fun stopPackageWatch() {
        packageWatchJob?.cancel()
        packageWatchJob = null
    }

    fun initializePreferences() {
        _uiState.update {
            it.copy(
                showSystemApps = settingsRepo.superuserShowSystemApps,
                showOnlyPrimaryUserApps = settingsRepo.superuserShowOnlyPrimaryUserApps,
                sortConfig = AppSortConfig.fromInt(settingsRepo.superuserSortOption),
            )
        }
    }

    fun updateSortConfig(config: AppSortConfig): Job {
        settingsRepo.superuserSortOption = config.toInt()
        _uiState.update { it.copy(sortConfig = config) }
        sortJob?.cancel()
        return viewModelScope.launch {
            val current = _uiState.value.groupedApps
            if (current.isEmpty()) return@launch
            updateVisibleApps(current)
        }.also { sortJob = it }
    }

    private fun refilterVisibleApps(): Job = viewModelScope.launch {
        // Re-filter when a filter setting changes
        val grouped = withContext(Dispatchers.IO) {
            buildGroups(filterApps(apps))
        }
        updateVisibleApps(grouped)
    }

    fun toggleShowSystemApps(): Job {
        val newValue = !_uiState.value.showSystemApps
        settingsRepo.superuserShowSystemApps = newValue
        _uiState.update { it.copy(showSystemApps = newValue) }
        return refilterVisibleApps()
    }

    fun toggleShowOnlyPrimaryUserApps(): Job {
        val newValue = !_uiState.value.showOnlyPrimaryUserApps
        settingsRepo.superuserShowOnlyPrimaryUserApps = newValue
        _uiState.update { it.copy(showOnlyPrimaryUserApps = newValue) }
        return refilterVisibleApps()
    }

    fun updateSearchStatus(status: SearchStatus) {
        val previous = _uiState.value.searchStatus
        _uiState.update { it.copy(searchStatus = status) }
        if (previous.searchText != status.searchText) {
            searchQuery.value = status.searchText
        }
    }

    fun updateSearchText(text: String) {
        updateSearchStatus(_uiState.value.searchStatus.copy(searchText = text))
    }

    private fun filterSearchResults(groups: List<GroupedApps>, text: String): List<GroupedApps> {
        if (text.isEmpty()) return emptyList()

        return groups.mapNotNull { group ->
            val matchedIdentifiers = group.apps.filter {
                it.label.contains(text, true) ||
                        it.displayIdentifier.contains(text, true) ||
                        PinyinUtil.toPinyin(it.label).contains(text, true)
            }.mapTo(linkedSetOf()) { it.displayIdentifier }

            if (matchedIdentifiers.isEmpty()) {
                null
            } else {
                val sortedApps = group.apps.sortedWith(
                    compareByDescending { it.displayIdentifier in matchedIdentifiers }
                )
                group.copy(
                    apps = sortedApps,
                    matchedIdentifiers = matchedIdentifiers,
                )
            }
        }
    }

    private suspend fun applySearchText(text: String) {
        _uiState.update {
            it.copy(
                searchStatus = it.searchStatus.copy(
                    resultStatus = searchLoadingStatusFor(text)
                )
            )
        }

        if (text.isEmpty()) {
            _uiState.update { state ->
                state.copy(
                    searchResults = emptyList(),
                    searchStatus = state.searchStatus.copy(resultStatus = SearchStatus.ResultStatus.DEFAULT)
                )
            }
            return
        }

        val result = withContext(Dispatchers.IO) {
            filterSearchResults(_uiState.value.groupedApps, text)
        }

        _uiState.update {
            it.copy(
                searchResults = result,
                searchStatus = it.searchStatus.copy(resultStatus = searchResultStatusFor(text, result.isEmpty()))
            )
        }
    }

    private fun updateCachedGroupedApps(grouped: List<GroupedApps>) {
        synchronized(groupedAppsLock) {
            cachedGroupedApps = grouped
        }
    }

    private suspend fun updateVisibleApps(grouped: List<GroupedApps>, resort: Boolean = true) {
        val sortConfig = _uiState.value.sortConfig
        val searchText = _uiState.value.searchStatus.searchText
        val (sorted, searchResults, recentlyInstalledResults) = withContext(Dispatchers.IO) {
            val s = if (resort) sortGroups(grouped, sortConfig) else grouped
            Triple(s, filterSearchResults(s, searchText), buildRecentlyInstalledGroups(s))
        }
        _uiState.update {
            it.copy(
                groupedApps = sorted,
                recentlyInstalledResults = recentlyInstalledResults,
                searchResults = searchResults,
                searchStatus = it.searchStatus.copy(
                    resultStatus = searchResultStatusFor(searchText, searchResults.isEmpty())
                )
            )
        }
    }

    private fun filterApps(list: List<AppInfo>): List<AppInfo> {
        val currentState = _uiState.value

        return list.filter {
            if (it.packageName == ksuApp.packageName) return@filter false
            // UID 1053 is the single system-wide WebView zygote (primary user only).
            if (it.isWebViewZygote) return@filter true
            if (it.allowSu || it.hasCustomProfile) {
                return@filter true
            }
            val userFilter = !currentState.showOnlyPrimaryUserApps || it.uid / 100000 == 0
            val isSystemApp = it.packageInfo.applicationInfo!!.flags.and(ApplicationInfo.FLAG_SYSTEM) != 0
            val typeFilter = it.uid == 2000
                    || currentState.showSystemApps
                    || !isSystemApp
            userFilter && typeFilter
        }
    }

    private fun buildCachedGroups(apps: List<AppInfo>): List<GroupedApps> {
        return buildGroups(apps.filter { it.packageName != ksuApp.packageName })
    }

    private fun buildGroups(apps: List<AppInfo>): List<GroupedApps> {
        val collator = Collator.getInstance(Locale.getDefault())
        val comparator = compareBy<AppInfo> {
            when {
                it.allowSu -> 0
                it.hasCustomProfile -> 1
                else -> 2
            }
        }.thenBy(collator) { it.label }
        return apps.groupBy { it.uid }.map { (uid, list) ->
            val sorted = list.sortedWith(comparator)
            val primary = pickPrimary(sorted)
            val shouldUmount = Natives.uidShouldUmount(uid)
            val ownerName = if (sorted.size > 1) ownerNameForUid(uid, sorted) else null

            GroupedApps(
                uid = uid,
                apps = sorted,
                primary = primary,
                anyAllowSu = sorted.any { it.allowSu },
                anyCustom = sorted.any { it.hasCustomProfile },
                shouldUmount = shouldUmount,
                ownerName = ownerName
            )
        }
    }

    // Derive visible groups from the already sorted, complete UID groups.
    // Identity membership avoids hashing AppInfo's potentially large profile.
    // Iterate UIDs in the original filtered order to preserve stable sort ties.
    private fun buildVisibleGroups(cached: List<GroupedApps>, apps: List<AppInfo>): List<GroupedApps> {
        val visible = filterApps(apps)
        val visibleSet = Collections.newSetFromMap(IdentityHashMap<AppInfo, Boolean>())
        visibleSet.addAll(visible)
        val byUid = cached.associateBy { it.uid }
        return visible.asSequence().map { it.uid }.distinct().map { uid ->
            val group = byUid.getValue(uid)
            if (group.apps.size == 1) {
                group
            } else {
                val subset = group.apps.filter { it in visibleSet }
                if (subset.size == group.apps.size) group else group.copy(
                    apps = subset,
                    primary = pickPrimary(subset),
                    anyAllowSu = subset.any { it.allowSu },
                    anyCustom = subset.any { it.hasCustomProfile },
                    ownerName = if (subset.size > 1) ownerNameForUid(uid, subset) else null,
                )
            }
        }.toList()
    }

    private fun groupRank(group: GroupedApps): Int = when {
        group.anyAllowSu -> 0
        group.anyCustom -> 1
        group.apps.size > 1 -> 2
        else -> 3
    }

    private fun sortGroups(groups: List<GroupedApps>, config: AppSortConfig): List<GroupedApps> {
        val collator = Collator.getInstance(Locale.getDefault())
        val base: Comparator<GroupedApps> = when (config.sortType) {
            AppSortType.PACKAGE_NAME -> compareBy { it.primary.displayIdentifier }
            AppSortType.INSTALL_TIME -> compareBy { it.primary.packageInfo.firstInstallTime }
            AppSortType.UPDATE_TIME -> compareBy { it.primary.packageInfo.lastUpdateTime }
            AppSortType.NAME -> Comparator { a, b -> collator.compare(a.primary.label, b.primary.label) }
        }
        val secondary = if (config.reversed) base.reversed() else base

        return groups.sortedWith(Comparator { a, b ->
            val ra = groupRank(a)
            val rb = groupRank(b)
            if (ra != rb) ra - rb else secondary.compare(a, b)
        })
    }

    suspend fun fetchAppList(silent: Boolean = false) {
        refreshMutex.withLock {
            if (silent && _uiState.value.hasLoaded && scannedPackageSeq == PackageEvents.sequence.value) return
            if (!silent) _uiState.update { it.copy(isRefreshing = true, error = null) }

            loadAppSnapshot(useCache = silent, repo = repo).onSuccess { snapshot ->
                val (cachedGroups, grouped) = withContext(Dispatchers.IO) {
                    val cached = buildCachedGroups(snapshot.apps)
                    cached to buildVisibleGroups(cached, snapshot.apps)
                }

                updateCachedGroupedApps(cachedGroups)
                updateVisibleApps(grouped)
                scannedPackageSeq = snapshot.packageSeq
                _uiState.update { it.copy(userIds = snapshot.userIds, isRefreshing = false, hasLoaded = true) }
            }.onFailure { e ->
                if (!silent) _uiState.update {
                    it.copy(
                        isRefreshing = false,
                        hasLoaded = true,
                        error = e
                    )
                }
            }

            isNeedRefresh = false
        }
    }

    private suspend fun refreshAppList(resort: Boolean = true) {
        var needsScan = false
        refreshMutex.withLock {
            val (currentApps, packageSeq) = synchronized(appsLock) { cachedApps to cachedPackageSeq }
            if (packageSeq != PackageEvents.sequence.value) {
                needsScan = true
                return@withLock
            }
            if (currentApps.isEmpty()) return

            repo.refreshProfiles(currentApps).onSuccess { updatedApps ->
                // A profile refresh based on an older inventory must not replace it.
                val published = synchronized(appsLock) {
                    if (cachedApps !== currentApps || cachedPackageSeq != packageSeq ||
                        PackageEvents.sequence.value != packageSeq) {
                        false
                    } else {
                        cachedApps = updatedApps
                        true
                    }
                }
                if (!published) {
                    needsScan = true
                    return@onSuccess
                }

                val (cachedGroups, grouped) = withContext(Dispatchers.IO) {
                    val cached = buildCachedGroups(updatedApps)
                    val visible = buildVisibleGroups(cached, updatedApps)
                    val result = if (resort) {
                        visible
                    } else {
                        val byUid = visible.associateBy { it.uid }
                        _uiState.value.groupedApps.map { group ->
                            byUid[group.uid] ?: group.copy(shouldUmount = Natives.uidShouldUmount(group.uid))
                        }
                    }
                    cached to result
                }
                updateCachedGroupedApps(cachedGroups)

                updateVisibleApps(grouped, resort = resort)
                _uiState.update { it.copy(isRefreshing = false) }
                isNeedRefresh = false
            }
        }
        // Release refreshMutex before requesting a full inventory refresh.
        if (needsScan) fetchAppList()
    }

    fun loadAppList(force: Boolean = false, resort: Boolean = true): Job {
        return viewModelScope.launch {
            if (force || _uiState.value.groupedApps.isEmpty()) {
                fetchAppList()
            } else {
                refreshAppList(resort)
            }
        }
    }

}
