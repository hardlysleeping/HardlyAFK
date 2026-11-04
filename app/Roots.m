//
//  Roots.m
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import <FileProvider/FileProvider.h>
#import "Roots.h"
#import "AppGroup.h"
#import "NSObject+SaneKVO.h"
#include "tools/fakefs.h"
#include "core/environment/afk_ios_boot.h"
#include "tools/afk_managed_install.h"
#include <sys/stat.h>
#include <errno.h>

static NSURL *ManagedStoreURL(void) {
    return [ContainerURL() URLByAppendingPathComponent:@"managed-environments" isDirectory:YES];
}
static BOOL ManagedError(NSError **error, const char *message) {
    if (error) *error = [NSError errorWithDomain:@"HardlyAFK.Environments" code:1
        userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:message]}];
    return NO;
}
static struct afk_catalog *OpenManagedCatalog(NSError **error) {
    NSError *directoryError = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:ManagedStoreURL()
        withIntermediateDirectories:YES attributes:nil error:&directoryError]) {
        if (error) *error = directoryError;
        return NULL;
    }
    struct stat st;
    if (lstat(ManagedStoreURL().fileSystemRepresentation, &st) || !S_ISDIR(st.st_mode)) {
        ManagedError(error, "Managed store must be a real directory, not a symlink");
        return NULL;
    }
    NSURL *database = [ManagedStoreURL() URLByAppendingPathComponent:@"catalog.db"];
    int exists = lstat(database.fileSystemRepresentation, &st);
    if ((!exists && !S_ISREG(st.st_mode)) || (exists && errno != ENOENT)) {
        ManagedError(error, "Managed catalog must be a regular file, not a symlink");
        return NULL;
    }
    struct afk_catalog *catalog = NULL;
    char message[512] = "Could not open environment catalog";
    if (afk_catalog_open(ManagedStoreURL().fileSystemRepresentation, &catalog, message, sizeof(message)) < 0)
        ManagedError(error, message);
    return catalog;
}
static void CollectManagedEnvironment(const struct afk_env_record *env, void *context) {
    NSMutableArray *rows = (__bridge NSMutableArray *)context;
    [rows addObject:@{@"id": [NSString stringWithUTF8String:env->id], @"name": [NSString stringWithUTF8String:env->name],
                     @"detail": [NSString stringWithFormat:@"%s %s · %s", env->distro_id, env->distro_version, env->architecture]}];
}

static NSURL *RootsDir(void) {
    static NSURL *rootsDir;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        rootsDir = [ContainerURL() URLByAppendingPathComponent:@"roots"];
        NSFileManager *manager = [NSFileManager defaultManager];
        [manager createDirectoryAtURL:rootsDir
          withIntermediateDirectories:YES
                           attributes:@{}
                                error:nil];
    });
    return rootsDir;
}

static NSString *kDefaultRoot = @"Default Root";

@interface Roots ()
@property NSMutableOrderedSet<NSString *> *roots;
@property BOOL updatingDomains;
@property BOOL domainsNeedUpdate;
@property BOOL wantsVersionFile;
@end

@implementation Roots

- (NSString *)managedSelection {
    return [NSUserDefaults.standardUserDefaults stringForKey:@"AFK Next Managed Environment"] ?: @"";
}

- (NSArray<NSDictionary<NSString *, NSString *> *> *)managedEnvironments:(NSError **)error {
    struct afk_catalog *catalog = OpenManagedCatalog(error);
    if (!catalog) return @[];
    NSMutableArray *rows = [NSMutableArray array];
    int count = afk_catalog_list(catalog, CollectManagedEnvironment, (__bridge void *)rows);
    afk_catalog_close(catalog);
    if (count < 0) {
        ManagedError(error, "Could not list managed environments");
        return @[];
    }
    return rows;
}

- (BOOL)createManagedAlpineNamed:(NSString *)name error:(NSError **)error {
#if defined(GUEST_ARM64) && !ISH_LINUX
    // Only a new UUID tree is touched. Unlike the lifecycle install API, this
    // isolated bundled provisioner never pretends the process guest is stopped.
    // Serialize conversions without holding the main thread or remounting Linux.
    @synchronized (self) {
        NSURL *archive = [NSBundle.mainBundle URLForResource:@"root" withExtension:@"tar.gz"];
        if (!archive) return ManagedError(error, "Bundled Alpine archive is missing");
        struct afk_catalog *catalog = OpenManagedCatalog(error);
        if (!catalog) return NO;
        char message[512] = {0};
        int rc = afk_managed_install(catalog, afk_distro_find("alpine", "3.24"),
            archive.fileSystemRepresentation, name.UTF8String, (struct progress){0},
            NULL, message, sizeof(message));
        afk_catalog_close(catalog);
        return rc == 0 ? YES : ManagedError(error, message);
    }
#else
    return ManagedError(error, "Managed creation requires the ARM64 emulator app target");
#endif
}

- (BOOL)selectManagedEnvironment:(NSString *)identifier error:(NSError **)error {
    if (identifier.length) {
#if defined(GUEST_ARM64) && !ISH_LINUX
        struct afk_catalog *catalog = OpenManagedCatalog(error);
        if (!catalog) return NO;
        char path[AFK_ENV_PATH_MAX], message[512];
        int result = afk_ios_boot_root(catalog, identifier.UTF8String, path, message, sizeof(message));
        afk_catalog_close(catalog);
        if (result < 0) return ManagedError(error, message);
#else
        return ManagedError(error, "Managed boot requires the ARM64 emulator app target");
#endif
    }
    // Never change catalog active/runtime state while an existing guest may be running.
    [NSUserDefaults.standardUserDefaults setObject:identifier forKey:@"AFK Next Managed Environment"];
    return YES;
}

- (NSURL *)bootRootURL:(NSError **)error {
    // Capture exactly once per process; changing selection cannot retarget a live mount.
    static NSURL *bootRoot;
    static NSError *bootFailure;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (!self.managedSelection.length) {
            bootRoot = [self rootUrl:self.defaultRoot];
            return;
        }
#if defined(GUEST_ARM64) && !ISH_LINUX
        NSError *failure = nil;
        struct afk_catalog *catalog = OpenManagedCatalog(&failure);
        if (!catalog) {
            bootFailure = failure;
            return;
        }
        char path[AFK_ENV_PATH_MAX], message[512];
        if (afk_ios_boot_root(catalog, self.managedSelection.UTF8String, path, message, sizeof(message)) == 0 &&
            afk_catalog_set_active(catalog, self.managedSelection.UTF8String, message, sizeof(message)) == 0)
            bootRoot = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path] isDirectory:YES];
        else
            ManagedError(&failure, message);
        afk_catalog_close(catalog);
        bootFailure = failure;
#else
        bootFailure = [NSError errorWithDomain:@"HardlyAFK.Environments" code:1
            userInfo:@{NSLocalizedDescriptionKey: @"Managed boot requires the ARM64 emulator app target"}];
#endif
    });
    if (error) *error = bootFailure;
    return bootRoot;
}

- (instancetype)init {
    if (self = [super init]) {
        NSError *error = nil;
        NSArray<NSString *> *rootNames = [NSFileManager.defaultManager contentsOfDirectoryAtPath:RootsDir().path error:&error];
        NSAssert(error == nil, @"couldn't list roots: %@", error);
        self.roots = [rootNames mutableCopy];
        if (!self.roots.count) {
            // import default root
            NSError *error;
            if (![self importRootFromArchive:[NSBundle.mainBundle URLForResource:@"root" withExtension:@"tar.gz"]
                                        name:@"default"
                                       error:&error
                            progressReporter:nil]) {
                NSAssert(NO, @"failed to import default root, error %@", error);
            }
            _wantsVersionFile = YES;
        }
        [self observe:@[@"roots"] options:0 owner:self usingBlock:^(typeof(self) self) {
            if (self.defaultRoot == nil && self.roots.count)
                self.defaultRoot = self.roots[0];
            [self syncFileProviderDomains];
        }];
        [self syncFileProviderDomains];

        if ((!self.defaultRoot || ![self.roots containsObject:self.defaultRoot]) && self.roots.count)
            self.defaultRoot = self.roots.firstObject;
    }
    return self;
}

- (NSString *)defaultRoot {
    return [NSUserDefaults.standardUserDefaults stringForKey:kDefaultRoot];
}
- (void)setDefaultRoot:(NSString *)defaultRoot {
    [NSUserDefaults.standardUserDefaults setObject:defaultRoot forKey:kDefaultRoot];
}

- (NSURL *)rootUrl:(NSString *)name {
    return [RootsDir() URLByAppendingPathComponent:name];
}

- (void)syncFileProviderDomains {
    if (self.updatingDomains) {
        self.domainsNeedUpdate = YES;
        return;
    }
    self.updatingDomains = YES;
    self.domainsNeedUpdate = NO;

    [NSFileProviderManager getDomainsWithCompletionHandler:^(NSArray<NSFileProviderDomain *> *domains, NSError *error) {
        void (^onError)(NSError *error) = ^(NSError *error) {
            if (error != nil)
                NSLog(@"error adjusting domains: %@", error);
        };
        onError(error);
        NSMutableOrderedSet<NSString *> *missingRoots = [self.roots mutableCopy];
        for (NSFileProviderDomain *domain in domains) {
            if ([missingRoots containsObject:domain.identifier]) {
                [missingRoots removeObject:domain.identifier];
            } else {
                [NSFileManager.defaultManager removeItemAtURL:
                 [NSFileProviderManager.defaultManager.documentStorageURL
                  URLByAppendingPathComponent:domain.pathRelativeToDocumentStorage]
                                                        error:nil];
                [NSFileProviderManager removeDomain:domain completionHandler:onError];
            }
        }
        for (NSString *rootId in missingRoots) {
            [NSFileProviderManager addDomain:[[NSFileProviderDomain alloc] initWithIdentifier:rootId
                                                                                  displayName:rootId
                                                                pathRelativeToDocumentStorage:rootId]
                           completionHandler:onError];
        }
        if (self.domainsNeedUpdate)
            [self syncFileProviderDomains];
        self.updatingDomains = NO;
    }];
}

- (BOOL)accessInstanceVariablesDirectly {
    return YES;
}

void root_progress_callback(void *cookie, double progress, const char *message, bool *should_cancel) {
    id <ProgressReporter> reporter = (__bridge id<ProgressReporter>) cookie;
    [reporter updateProgress:progress message:[NSString stringWithUTF8String:message]];
    if ([reporter shouldCancel])
        *should_cancel = true;
}

- (BOOL)importRootFromArchive:(NSURL *)archive name:(NSString *)name error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
    NSAssert(![self.roots containsObject:name], @"root already exists: %@", name);
    struct fakefsify_error fs_err;
    NSURL *destination = [self rootUrl:name];
    NSURL *tempDestination = [NSFileManager.defaultManager.temporaryDirectory
                              URLByAppendingPathComponent:[NSProcessInfo.processInfo globallyUniqueString]];
    if (tempDestination == nil)
        return NO;
    if (!fakefs_import(archive.fileSystemRepresentation,
                       tempDestination.fileSystemRepresentation,
                       &fs_err, (struct progress) {(__bridge void *) progress, root_progress_callback})) {
        NSString *domain = NSPOSIXErrorDomain;
        if (fs_err.type == ERR_SQLITE)
            domain = @"SQLite";
        *error = [NSError errorWithDomain:domain
                                     code:fs_err.code
                                 userInfo:@{NSLocalizedDescriptionKey:
                                                [NSString stringWithFormat:@"%s, line %d", fs_err.message, fs_err.line]}];
        if (fs_err.type == ERR_CANCELLED)
            *error = nil;
        free(fs_err.message);
        [NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];
        return NO;
    }
    if (![NSFileManager.defaultManager moveItemAtURL:tempDestination toURL:destination error:error])
        return NO;

    void (^addRoot)(void) = ^{
        [[self mutableOrderedSetValueForKey:@"roots"] addObject:name];
    };
    if (!NSThread.isMainThread)
        dispatch_sync(dispatch_get_main_queue(), addRoot);
    else
        addRoot();
    return YES;
}

- (BOOL)exportRootNamed:(NSString *)name toArchive:(NSURL *)archive error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
    NSAssert([self.roots containsObject:name], @"trying to export a root that doesn't exist: %@", name);
    struct fakefsify_error fs_err;
    if (!fakefs_export([self rootUrl:name].fileSystemRepresentation,
                       archive.fileSystemRepresentation,
                       &fs_err, (struct progress) {(__bridge void *) progress, root_progress_callback})) {
        // TODO: dedup with above method
        NSString *domain = NSPOSIXErrorDomain;
        if (fs_err.type == ERR_SQLITE)
            domain = @"SQLite";
        *error = [NSError errorWithDomain:domain
                                     code:fs_err.code
                                 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:fs_err.message]}];
        if (fs_err.type == ERR_CANCELLED)
            *error = nil;
        free(fs_err.message);
        return NO;
    }
    return YES;
}

- (BOOL)destroyRootNamed:(NSString *)name error:(NSError **)error {
    if ([name isEqualToString:self.defaultRoot]) {
        *error = [NSError errorWithDomain:@"ios-linuxkit" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Cannot delete the default filesystem"}];
        return NO;
    }
    NSAssert([self.roots containsObject:name], @"root does not exist: %@", name);
    if (![NSFileManager.defaultManager removeItemAtURL:[self rootUrl:name] error:error])
        return NO;
    [[self mutableOrderedSetValueForKey:@"roots"] removeObject:name];
    return YES;
}

- (BOOL)renameRoot:(NSString *)name toName:(NSString *)newName error:(NSError **)error {
    if (name.length == 0) {
        *error = [NSError errorWithDomain:@"ios-linuxkit" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't be empty"}];
        return NO;
    }
    if ([name containsString:@"/"]) {
        *error = [NSError errorWithDomain:@"ios-linuxkit" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't contain /"}];
        return NO;
    }
    if ([name isEqualToString:@"."] || [name isEqualToString:@".."]) {
        *error = [NSError errorWithDomain:@"ios-linuxkit" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't be . or .."}];
        return NO;
    }
    if ([name isEqualToString:self.defaultRoot]) {
        *error = [NSError errorWithDomain:@"ios-linuxkit" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Cannot rename the default filesystem"}];
        return NO;
    }
    NSAssert([self.roots containsObject:name], @"root does not exist: %@", name);
    
    if (![NSFileManager.defaultManager moveItemAtURL:[self rootUrl:name] toURL:[self rootUrl:newName] error:error])
        return NO;
    NSUInteger index = [self.roots indexOfObject:name];
    [[self mutableOrderedSetValueForKey:@"roots"] replaceObjectAtIndex:index withObject:newName];
    return YES;
}

+ (instancetype)instance {
    static Roots *instance;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        instance = [Roots new];
    });
    return instance;
}

@end
