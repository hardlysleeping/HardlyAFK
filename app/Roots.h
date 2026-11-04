//
//  Roots.h
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@protocol ProgressReporter

- (void)updateProgress:(double)progressFraction message:(NSString *)progressMessage;
- (BOOL)shouldCancel;

@end

@interface Roots : NSObject

+ (instancetype)instance;

@property (readonly) NSOrderedSet<NSString *> *roots;
@property NSString *defaultRoot;
@property (readonly) BOOL wantsVersionFile;
- (NSURL *)rootUrl:(NSString *)name;
// Selection is next-process only. nil boot URL means fail closed, never fallback.
- (NSArray<NSDictionary<NSString *, NSString *> *> *)managedEnvironments:(NSError **)error;
- (BOOL)selectManagedEnvironment:(NSString *)identifier error:(NSError **)error;
// Creates an independent fakefs from the verified bundled Alpine; no live mount changes.
- (BOOL)createManagedAlpineNamed:(NSString *)name error:(NSError **)error;
- (nullable NSURL *)bootRootURL:(NSError **)error;
@property (readonly) NSString *managedSelection;
- (BOOL)importRootFromArchive:(NSURL *)archive name:(NSString *)name error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress;
- (BOOL)exportRootNamed:(NSString *)name toArchive:(NSURL *)archive error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress;
- (BOOL)destroyRootNamed:(NSString *)name error:(NSError **)error;
- (BOOL)renameRoot:(NSString *)name toName:(NSString *)newName error:(NSError **)error;

@end

NS_ASSUME_NONNULL_END
