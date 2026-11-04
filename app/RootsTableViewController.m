//
//  RootsTableViewController.m
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import "Roots.h"
#import "RootsTableViewController.h"
#import "ProgressReportViewController.h"
#import "UIApplication+OpenURL.h"
#import "UIViewController+Extras.h"
#import "NSObject+SaneKVO.h"

@interface ManagedEnvironmentsViewController : UITableViewController
@property (nonatomic, copy) NSArray<NSDictionary<NSString *, NSString *> *> *environments;
@property (nonatomic, copy) NSString *loadError;
@property (nonatomic) BOOL creating;
@property (nonatomic) NSUInteger reloadGeneration;
- (void)reloadEnvironments;
- (void)createEnvironment;
@end

@interface RootsTableViewController ()
@end

@interface RootDetailViewController : UITableViewController <UIDocumentPickerDelegate, UITextFieldDelegate>

@property (nonatomic) NSString *rootName;
@property (nonatomic) NSURL *exportURL;

@property (weak, nonatomic) IBOutlet UITextField *nameField;
@property (weak, nonatomic) IBOutlet UILabel *deleteLabel;
@property (weak, nonatomic) IBOutlet UITableViewCell *deleteCell;

@end

@implementation RootsTableViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    UIButton *managed = [UIButton buttonWithType:UIButtonTypeSystem];
    managed.frame = CGRectMake(0, 0, self.tableView.bounds.size.width, 52);
    [managed setTitle:@"Managed Environments…" forState:UIControlStateNormal];
    [managed addTarget:self action:@selector(showManagedEnvironments) forControlEvents:UIControlEventTouchUpInside];
    self.tableView.tableHeaderView = managed;
    [Roots.instance observe:@[@"roots", @"defaultRoot"]
                    options:0 owner:self usingBlock:^(typeof(self) self) {
        [self.tableView reloadData];
    }];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return 1;
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return Roots.instance.roots.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    NSString *ident = @"Root";
    if ([Roots.instance.roots[indexPath.row] isEqual:Roots.instance.defaultRoot])
        ident = @"Default Root";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:ident forIndexPath:indexPath];
    cell.textLabel.text = Roots.instance.roots[indexPath.row];
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
}

- (void)prepareForSegue:(UIStoryboardSegue *)segue sender:(id)sender {
    RootDetailViewController *vc = segue.destinationViewController;
    vc.rootName = Roots.instance.roots[self.tableView.indexPathForSelectedRow.row];
}

- (void)showManagedEnvironments {
    [self.navigationController pushViewController:[[ManagedEnvironmentsViewController alloc]
        initWithStyle:UITableViewStyleGrouped] animated:YES];
}

- (IBAction)importFilesystem:(id)sender {
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
                                              initWithDocumentTypes:@[@"public.tar-archive", @"org.gnu.gnu-zip-archive"]
                                              inMode:UIDocumentPickerModeImport];
    [self presentViewController:picker animated:YES completion:nil];
    if (@available(iOS 13, *)) {
        picker.shouldShowFileExtensions = YES;
    }
    picker.delegate = self;
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSAssert(urls.count == 1, @"somehow picked multiple documents");
    NSURL *url = urls.firstObject;
    NSString *fileName = url.lastPathComponent.stringByDeletingPathExtension;
    if ([fileName hasSuffix:@".tar"])
        fileName = fileName.stringByDeletingPathExtension;
    unsigned i = 2;
    NSString *name = fileName;
    while ([Roots.instance.roots containsObject:name]) {
        name = [NSString stringWithFormat:@"%@ %u", fileName, i++];
    }

    ProgressReportViewController *progressVC = [self.storyboard instantiateViewControllerWithIdentifier:@"progress"];
    progressVC.title = [NSString stringWithFormat:@"Importing %@", name];
    [self presentViewController:progressVC animated:YES completion:nil];

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error;
        [url startAccessingSecurityScopedResource];
        BOOL success = [Roots.instance importRootFromArchive:url name:name error:&error progressReporter:progressVC];
        [url stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(), ^{
            [progressVC dismissViewControllerAnimated:YES completion:^{
                if (!success && error != nil)
                    [self presentError:error title:@"Import failed"];
            }];
        });
    });
}

@end

@implementation RootDetailViewController

- (void)viewWillAppear:(BOOL)animated {
    self.nameField.text = self.rootName;
    [self update];
}

- (void)update {
    self.navigationItem.title = self.rootName;
    self.nameField.enabled = !self.isDefaultRoot;
    self.nameField.clearButtonMode = self.isDefaultRoot ? UITextFieldViewModeNever : UITextFieldViewModeAlways;
    self.deleteLabel.enabled = !self.isDefaultRoot;
    self.deleteCell.selectionStyle = !self.isDefaultRoot ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
    [self.tableView reloadData];
}

- (IBAction)nameChanged:(id)sender {
    NSString *newName = self.nameField.text;
    NSError *err;
    if (![Roots.instance renameRoot:self.rootName toName:newName error:&err]) {
        self.nameField.text = self.rootName;
        [self presentError:err title:@"Rename failed"];
        return;
    }
    self.rootName = newName;
    [self update];
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    [textField resignFirstResponder];
    return NO;
}

- (BOOL)isDefaultRoot {
    return [self.rootName isEqualToString:Roots.instance.defaultRoot];
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == 2) { // delete
        if (self.isDefaultRoot)
            return @"This filesystem can't be deleted because it's currently mounted as the root.";
    }
    return [super tableView:tableView titleForFooterInSection:section];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == 0 && indexPath.row == 1)
        [self browseFiles];
    if (indexPath.section == 0 && indexPath.row == 2)
        [self exportFilesystem];
    if (indexPath.section == 1 && indexPath.row == 0)
        [self bootThis];
    if (indexPath.section == 2 && indexPath.row == 0)
        [self deleteFilesystem];
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
}

- (void)browseFiles {
    NSURL *url = [NSFileProviderManager.defaultManager.documentStorageURL URLByAppendingPathComponent:self.rootName];
    NSURLComponents *components = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
    components.scheme = @"shareddocuments";
    [UIApplication openURL:components.string];
}

- (void)exportFilesystem {
    self.exportURL = [[NSFileManager.defaultManager.temporaryDirectory
                       URLByAppendingPathComponent:[NSProcessInfo.processInfo globallyUniqueString]]
                      URLByAppendingPathComponent:[NSString stringWithFormat:@"%@.tar.gz", self.rootName]];
    [NSFileManager.defaultManager createDirectoryAtURL:self.exportURL.URLByDeletingLastPathComponent
                           withIntermediateDirectories:YES
                                            attributes:nil
                                                 error:nil];
    ProgressReportViewController *progressVC = [self.storyboard instantiateViewControllerWithIdentifier:@"progress"];
    progressVC.title = [NSString stringWithFormat:@"Exporting %@", self.rootName];
    [self presentViewController:progressVC animated:YES completion:nil];

    // witness the callback hell
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *err;
        BOOL success = [Roots.instance exportRootNamed:self.rootName toArchive:self.exportURL error:&err progressReporter:progressVC];
        dispatch_async(dispatch_get_main_queue(), ^{
            [progressVC dismissViewControllerAnimated:YES completion:^{
                if (!success) {
                    if (err != nil)
                        [self presentError:err title:@"Export failed"];
                    return;
                }

                UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
                                                          initWithURL:self.exportURL
                                                          inMode:UIDocumentPickerModeExportToService];
                picker.delegate = self;
                if (@available(iOS 13, *)) {
                    picker.shouldShowFileExtensions = YES;
                }
                [self presentViewController:picker animated:YES completion:nil];
            }];
        });
    });
}

- (void)setExportURL:(NSURL *)exportURL {
    [NSFileManager.defaultManager removeItemAtURL:_exportURL.URLByDeletingLastPathComponent error:nil];
    _exportURL = exportURL;
}

- (void)bootThis {
    Roots.instance.defaultRoot = self.rootName;
    exit(0);
}

- (void)deleteFilesystem {
    if (self.isDefaultRoot)
        return;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Really delete?"
                                                                   message:@"I can't be bothered to implement any undo or regret UI so this is irreversable."
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        NSError *error;
        if (![Roots.instance destroyRootNamed:self.rootName error:&error]) {
            [self presentError:error title:@"Delete failed"];
        } else {
            [self.navigationController popViewControllerAnimated:YES];
        }
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)dealloc {
    self.exportURL = nil; // get it deleted
}

@end

@implementation ManagedEnvironmentsViewController
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    self.title = @"Managed Environments";
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(createEnvironment)];
    self.navigationItem.rightBarButtonItem.enabled = !self.creating;
    [self reloadEnvironments];
}
- (void)reloadEnvironments {
    NSUInteger generation = ++self.reloadGeneration;
    // Catalog busy waits and fakefs validation must not block UIKit. Only the
    // newest request may publish its rows/error after an install or reappearance.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        NSArray *rows = [Roots.instance managedEnvironments:&error];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (generation != self.reloadGeneration) return;
            self.environments = rows;
            self.loadError = error.localizedDescription;
            [self.tableView reloadData];
        });
    });
}
- (void)createEnvironment {
    if (self.creating) return;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Create Alpine environment"
        message:@"Uses the bundled, SHA-256 verified Alpine 3.24 ARM64 archive. Creates independent fakefs data and metadata, not a copy of your running guest. No download, migration or live switch. Keep the app open until creation finishes."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.placeholder = @"Environment name";
        field.text = @"Alpine";
        field.autocapitalizationType = UITextAutocapitalizationTypeWords;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Create" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        NSString *name = [alert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (!name.length) {
            [alert dismissViewControllerAnimated:YES completion:^{
                [self presentError:[NSError errorWithDomain:@"HardlyAFK.Environments" code:1
                    userInfo:@{NSLocalizedDescriptionKey: @"Enter an environment name"}] title:@"Creation refused"];
            }];
            return;
        }
        self.creating = YES;
        self.navigationItem.rightBarButtonItem.enabled = NO;
        UIAlertController *busy = [UIAlertController alertControllerWithTitle:@"Creating Alpine…"
            message:@"Verifying archive and converting to fakefs. The running guest and next-launch selection stay unchanged."
            preferredStyle:UIAlertControllerStyleAlert];
        [alert dismissViewControllerAnimated:YES completion:^{
            [self presentViewController:busy animated:YES completion:^{
                dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                    NSError *error = nil;
                    BOOL success = [Roots.instance createManagedAlpineNamed:name error:&error];
                    dispatch_async(dispatch_get_main_queue(), ^{
                        self.creating = NO;
                        self.navigationItem.rightBarButtonItem.enabled = YES;
                        [self reloadEnvironments];
                        [busy dismissViewControllerAnimated:YES completion:^{
                            if (!success) [self presentError:error title:@"Creation failed"];
                        }];
                    });
                });
            }];
        }];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return self.environments.count + 1;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    return self.loadError ?: @"Checkmark = next launch, not the running guest. Selection never stops or switches Linux. Fully close the app before relaunching. Tap + to create an independent Alpine environment from the verified bundled archive. Legacy filesystems remain separate and unchanged. No migration, delete, export or network installer is provided.";
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    NSString *identifier = @"";
    if (indexPath.row == 0) {
        cell.textLabel.text = @"Legacy default filesystem";
        cell.detailTextLabel.text = Roots.instance.defaultRoot;
    } else {
        NSDictionary *row = self.environments[indexPath.row - 1];
        identifier = row[@"id"];
        cell.textLabel.text = row[@"name"];
        cell.detailTextLabel.text = row[@"detail"];
    }
    cell.accessoryType = [identifier isEqualToString:Roots.instance.managedSelection] ?
        UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    NSString *identifier = indexPath.row ? self.environments[indexPath.row - 1][@"id"] : @"";
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Use on next launch?"
        message:@"The current guest keeps running unchanged. Fully close HardlyAFK and reopen it to boot this selection. No automatic shutdown or safe-stop is claimed."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Select for next launch" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        NSError *error = nil;
        if (![Roots.instance selectManagedEnvironment:identifier error:&error])
            [self presentError:error title:@"Selection refused"];
        [self.tableView reloadData];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}
@end
