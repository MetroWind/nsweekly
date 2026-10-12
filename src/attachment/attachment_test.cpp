#include "attachment/attachment_module.hpp"
#include "attachment/image_variant.hpp"
#include "auth/auth_mock.hpp"
#include "storage_test_support.hpp"
#include "test_utils.hpp"
#include "user_sql.hpp"
#include <Magick++.h>
#include <fstream>
#include <future>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using testing::HasSubstr;
using testing::Return;

class Attachments : public testing::Test
{
protected:
    TemporaryDatabase temporary;
    Configuration config{};
    std::unique_ptr<SQLite> inspection;
    std::unique_ptr<AttachmentService> service;
    void SetUp() override
    {
        config.data_dir = std::filesystem::path(temporary.path()).parent_path();
        config.url_prefix = "http://localhost";
        ASSIGN_OR_FAIL(inspection, SQLite::connectFile(temporary.path()));
        ASSERT_TRUE(initializeUsers(*inspection));
        ASSERT_TRUE(inspection->execute(
            "INSERT INTO Users(id,name) VALUES(1,'one'),(2,'two')"));
        restart();
    }
    void restart()
    {
        service.reset();
        ASSIGN_OR_FAIL(auto connection, SQLite::connectFile(temporary.path()));
        service = std::make_unique<AttachmentService>(
            std::make_unique<AttachmentDataSqlite>(std::move(connection)),
            config);
        auto initialized = service->initialize();
        ASSERT_TRUE(initialized) << errorMsg(initialized.error());
    }
    E<AttachmentUpload> upload(int64_t user, std::string_view name,
                               std::string_view bytes)
    {
        ASSIGN_OR_RETURN(auto writer, service->beginUpload(user, name));
        for(size_t offset = 0; offset < bytes.size(); offset += 3)
        {
            DO_OR_RETURN(writer.append(bytes.substr(offset, 3)));
        }
        return writer.finish();
    }
    std::filesystem::path path(const std::string& hash)
    {
        return *attachmentPath(service->directory(), hash);
    }
    std::string bytes(const std::string& hash)
    {
        std::ifstream input(path(hash), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    }
    std::string image(const std::string& format = "PNG")
    {
        Magick::Image image(Magick::Geometry(20, 10), Magick::Color("none"));
        image.pixelColor(0, 0, Magick::Color("red"));
        Magick::Blob blob;
        image.write(&blob, format);
        return std::string(static_cast<const char*>(blob.data()),
                           blob.length());
    }
};

TEST(AttachmentIdentity, ValidatesMetadataAndEncodesWholeSegment)
{
    EXPECT_TRUE(attachmentHash("e3b0c44298fc1c149afbf4c8996fb924"));
    EXPECT_FALSE(attachmentHash("E3b0c44298fc1c149afbf4c8996fb924"));
    EXPECT_FALSE(attachmentHash("../file"));
    EXPECT_EQ(*attachmentName("C:\\fakepath\\ café 😀.pdf"), " café 😀.pdf");
    EXPECT_EQ(*attachmentName("/tmp/a b"), "a b");
    for(const auto name :
        {"", ".", "..", "bad\nname", "\xc0\xaf", "\xed\xa0\x80"})
    {
        EXPECT_FALSE(attachmentName(name)) << name;
    }
    EXPECT_FALSE(attachmentName(std::string("a\0b", 3)));
    EXPECT_FALSE(attachmentName(std::string(1025, 'a')));
    EXPECT_FALSE(attachmentName("bad\n/path/file"));
    EXPECT_EQ(attachmentSegment("a %?#\""), "a%20%25%3F%23%22");
}
TEST_F(Attachments, HashesEmptyBinaryAndChunks)
{
    ASSIGN_OR_FAIL(auto empty, upload(1, "empty", {}));
    EXPECT_EQ(empty.record.file.hash, "e3b0c44298fc1c149afbf4c8996fb924");
    ASSIGN_OR_FAIL(auto abc, upload(1, "abc", "abc"));
    EXPECT_EQ(abc.record.file.hash, "ba7816bf8f01cfea414140de5dae2223");
    EXPECT_EQ(path(abc.record.file.hash).parent_path().filename(), "ba");
    EXPECT_EQ(path(abc.record.file.hash).extension(), "");
    std::string binary("a\0b\xff", 4);
    ASSIGN_OR_FAIL(auto result, upload(1, "binary.txt", binary));
    EXPECT_EQ(bytes(result.record.file.hash), binary);
    EXPECT_EQ(result.record.file.byte_size, 4);
}
TEST_F(Attachments, DeduplicatesPerOwnerAndExactName)
{
    ASSIGN_OR_FAIL(auto first, upload(1, "café 😀", "same"));
    ASSIGN_OR_FAIL(auto again, upload(1, "café 😀", "same"));
    ASSIGN_OR_FAIL(auto renamed, upload(1, "Café 😀", "same"));
    ASSIGN_OR_FAIL(auto other, upload(2, "café 😀", "same"));
    EXPECT_FALSE(first.already_owned);
    EXPECT_TRUE(again.already_owned);
    EXPECT_FALSE(other.already_owned);
    EXPECT_EQ(first.record.id, again.record.id);
    EXPECT_EQ(first.record.added, again.record.added);
    EXPECT_NE(first.record.id, renamed.record.id);
    EXPECT_NE(first.record.id, other.record.id);
    EXPECT_EQ(first.record.file.hash, other.record.file.hash);
    EXPECT_EQ(service->listAttachments(1)->size(), 2);
    EXPECT_EQ(service->listAttachments(2)->size(), 1);
    EXPECT_FALSE(service->getAttachment(2, first.record.id));
    EXPECT_FALSE(service->deleteAttachment(2, first.record.id));
    EXPECT_FALSE(service->beginUpload(999, "file"));
    ASSERT_TRUE(service->deleteAttachment(1, first.record.id));
    ASSERT_TRUE(service->deleteAttachment(1, renamed.record.id));
    EXPECT_TRUE(std::filesystem::exists(path(first.record.file.hash)));
    ASSERT_TRUE(service->deleteAttachment(2, other.record.id));
    EXPECT_FALSE(std::filesystem::exists(path(first.record.file.hash)));
}
TEST_F(Attachments, CancellationAndLimitsLeaveNoRecords)
{
    {
        ASSIGN_OR_FAIL(auto writer, service->beginUpload(1, "partial"));
        ASSERT_TRUE(writer.append("partial"));
    }
    EXPECT_TRUE(std::filesystem::is_empty(service->directory() / ".staging"));
    EXPECT_TRUE(service->listAttachments(1)->empty());
    config.attachment_upload_limit = 2;
    restart();
    ASSIGN_OR_FAIL(auto writer, service->beginUpload(1, "limit"));
    ASSERT_TRUE(writer.append("ab"));
    auto rejected = writer.append("c");
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().as<HTTPError>()->code, 413);
    EXPECT_FALSE(writer.finish());
    EXPECT_TRUE(std::filesystem::is_empty(service->directory() / ".staging"));
    EXPECT_TRUE(service->listAttachments(1)->empty());
}
TEST_F(Attachments, ConcurrentUploadsShareBytesAndOneRecord)
{
    std::vector<std::future<E<AttachmentUpload>>> futures;
    for(int i = 0; i < 12; ++i)
    {
        futures.push_back(std::async(std::launch::async,
                                     [this]
                                     {
                                         return upload(1, "race", "concurrent");
                                     }));
    }
    int64_t id = 0;
    int inserted = 0;
    for(auto& future : futures)
    {
        ASSIGN_OR_FAIL(auto result, future.get());
        if(id == 0)
        {
            id = result.record.id;
        }
        EXPECT_EQ(id, result.record.id);
        inserted += !result.already_owned;
    }
    EXPECT_EQ(inserted, 1);
    EXPECT_EQ(service->listAttachments(1)->size(), 1);
}
TEST_F(Attachments, DatabaseFailureReclaimsProvisionalBytesImmediately)
{
    ASSERT_TRUE(inspection->execute(R"(
CREATE TRIGGER reject_attachment BEFORE INSERT ON Attachments
BEGIN SELECT RAISE(ABORT,'injected commit failure'); END;
)"));
    EXPECT_FALSE(upload(1, "failed", "abc"));
    const std::string hash = "ba7816bf8f01cfea414140de5dae2223";
    EXPECT_FALSE(std::filesystem::exists(path(hash)));
    EXPECT_TRUE(service->listAttachments(1)->empty());
    restart();
    EXPECT_FALSE(std::filesystem::exists(path(hash)));
}
TEST_F(Attachments, AdoptsVerifiedOrphanAndRejectsCorruptOne)
{
    const std::string hash = "ba7816bf8f01cfea414140de5dae2223";
    std::filesystem::create_directories(path(hash).parent_path());
    {
        std::ofstream output(path(hash));
        output << "wrong";
    }
    EXPECT_FALSE(upload(1, "file", "abc"));
    {
        std::ofstream output(path(hash));
        output << "abc";
    }
    ASSIGN_OR_FAIL(auto adopted, upload(1, "file", "abc"));
    EXPECT_EQ(adopted.record.file.byte_size, 3);
    EXPECT_EQ(bytes(hash), "abc");
}
TEST_F(Attachments, RestartRejectsMissingKnownBytes)
{
    ASSIGN_OR_FAIL(auto record, upload(1, "file", "abc"));
    std::filesystem::remove(path(record.record.file.hash));
    EXPECT_FALSE(service->initialize());
    EXPECT_EQ(service->listAttachments(1)->size(), 1);
}
TEST_F(Attachments, CleanupFailureCanBeRetriedAfterLogicalDeletion)
{
    ASSIGN_OR_FAIL(auto result, upload(1, "file", "abc"));
    auto published = path(result.record.file.hash);
    std::filesystem::remove(published);
    std::filesystem::create_directory(published);
    {
        std::ofstream obstacle(published / "obstacle");
        obstacle << "block unlink";
    }
    ASSERT_TRUE(service->deleteAttachment(1, result.record.id));
    EXPECT_TRUE(service->listAttachments(1)->empty());
    ASSIGN_OR_FAIL(auto pending, inspection->eval<int>(
                                     "SELECT count(*) FROM AttachmentCleanup"));
    EXPECT_EQ(std::get<0>(pending[0]), 1);
    std::filesystem::remove_all(published);
    ASSERT_TRUE(service->cleanup());
    ASSIGN_OR_FAIL(auto files, inspection->eval<int>(
                                   "SELECT count(*) FROM AttachmentFiles"));
    EXPECT_EQ(std::get<0>(files[0]), 0);
}
TEST_F(Attachments, ProbesContentsAndCachesVariants)
{
    auto original = image();
    ASSIGN_OR_FAIL(auto result, upload(1, "misleading.txt", original));
    EXPECT_EQ(result.record.file.mime_type, "image/png");
    ImageVariantOptions options{8, 8, 80, "webp"};
    ASSIGN_OR_FAIL(auto converted,
                   service->ensureImageVariant(1, result.record.id, options));
    EXPECT_EQ(converted.width, 8);
    EXPECT_EQ(converted.height, 4);
    EXPECT_EQ(converted.file.mime_type, "image/webp");
    EXPECT_THAT(converted.public_url, HasSubstr("misleading.webp"));
    ASSIGN_OR_FAIL(auto again,
                   service->ensureImageVariant(1, result.record.id, options));
    EXPECT_EQ(converted.file.hash, again.file.hash);
    EXPECT_EQ(bytes(result.record.file.hash), original);
    options.max_width = 100;
    options.max_height = 100;
    options.format = "png";
    ASSIGN_OR_FAIL(auto unscaled,
                   service->ensureImageVariant(1, result.record.id, options));
    EXPECT_EQ(unscaled.width, 20);
    EXPECT_EQ(unscaled.height, 10);
    Magick::Image decoded(path(unscaled.file.hash).string());
    EXPECT_TRUE(decoded.alpha());
    EXPECT_FALSE(service->ensureImageVariant(2, result.record.id, options));
    options.quality = 0;
    EXPECT_FALSE(service->ensureImageVariant(1, result.record.id, options));
}
TEST_F(Attachments, GeneratedBytesOwnedSeparatelySurviveParentDeletion)
{
    ASSIGN_OR_FAIL(auto result, upload(1, "source", image()));
    ASSIGN_OR_FAIL(auto variant, service->ensureImageVariant(
                                     1, result.record.id, {8, 8, 80, "jpeg"}));
    auto output = bytes(variant.file.hash);
    Magick::Image decoded(path(variant.file.hash).string());
    EXPECT_FALSE(decoded.alpha());
    ASSIGN_OR_FAIL(auto owned, upload(2, "generated.jpeg", output));
    ASSERT_TRUE(service->deleteAttachment(1, result.record.id));
    EXPECT_TRUE(std::filesystem::exists(path(variant.file.hash)));
    EXPECT_FALSE(std::filesystem::exists(path(result.record.file.hash)));
    ASSERT_TRUE(service->deleteAttachment(2, owned.record.id));
    EXPECT_FALSE(std::filesystem::exists(path(variant.file.hash)));
}
TEST_F(Attachments, RejectsUnsupportedAndCorruptConversionWithoutLosingOriginal)
{
    for(const auto contents :
        {"plain text", "<svg xmlns='http://www.w3.org/2000/svg'/>"})
    {
        ASSIGN_OR_FAIL(auto result, upload(1, "image.png", contents));
        EXPECT_FALSE(service->ensureImageVariant(1, result.record.id,
                                                 {8, 8, 80, "png"}));
        EXPECT_EQ(bytes(result.record.file.hash), contents);
    }
    auto contents = image();
    contents.resize(contents.size() / 2);
    ASSIGN_OR_FAIL(auto corrupt, upload(1, "broken", contents));
    EXPECT_FALSE(
        service->ensureImageVariant(1, corrupt.record.id, {8, 8, 80, "webp"}));
    EXPECT_EQ(bytes(corrupt.record.file.hash), contents);
    config.attachment_image_pixels = 10;
    restart();
    ASSIGN_OR_FAIL(auto large, upload(1, "large", image()));
    EXPECT_FALSE(
        service->ensureImageVariant(1, large.record.id, {8, 8, 80, "png"}));
}
TEST_F(Attachments, ParallelGenerationReusesRecipe)
{
    ASSIGN_OR_FAIL(auto result, upload(1, "source", image()));
    std::vector<std::future<E<ImageVariant>>> futures;
    for(int i = 0; i < 8; ++i)
    {
        futures.push_back(std::async(std::launch::async,
                                     [this, id = result.record.id]
                                     {
                                         return service->ensureImageVariant(
                                             1, id, {8, 8, 80, "webp"});
                                     }));
    }
    std::string hash;
    for(auto& future : futures)
    {
        ASSIGN_OR_FAIL(auto variant, future.get());
        if(hash.empty())
        {
            hash = variant.file.hash;
        }
        EXPECT_EQ(hash, variant.file.hash);
    }
    ASSIGN_OR_FAIL(
        auto mappings,
        inspection->eval<int>("SELECT count(*) FROM AttachmentVariants"));
    EXPECT_EQ(std::get<0>(mappings[0]), 1);
}
TEST_F(Attachments, ReclaimsSelfReferencingVariant)
{
    ASSIGN_OR_FAIL(auto result, upload(1, "self", "abc"));
    ASSIGN_OR_FAIL(
        auto sql,
        inspection->statementFromStr(
            "INSERT INTO AttachmentVariants VALUES(?,'self',?,1,1,'png',0)"));
    ASSERT_TRUE(sql.bind(result.record.file.hash, result.record.file.hash));
    ASSERT_TRUE(inspection->execute(std::move(sql)));
    ASSERT_TRUE(service->deleteAttachment(1, result.record.id));
    EXPECT_FALSE(std::filesystem::exists(path(result.record.file.hash)));
}

TEST_F(Attachments, OrientPortraitAndRejectAnimatedRaster)
{
    Magick::Image portrait(Magick::Geometry(20, 10), Magick::Color("red"));
    // A minimal little-endian EXIF IFD with orientation=6 (right/top).
    const unsigned char exif[] = {
        'E',  'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0,
        0x12, 1,   3,   0,   1, 0, 0,   0,   6,  0, 0, 0, 0, 0, 0, 0};
    portrait.profile("exif", Magick::Blob(exif, sizeof(exif)));
    portrait.orientation(Magick::RightTopOrientation);
    Magick::Blob blob;
    portrait.write(&blob, "JPEG");
    ASSIGN_OR_FAIL(auto original, upload(1, "portrait",
                                         {static_cast<const char*>(blob.data()),
                                          blob.length()}));
    ASSIGN_OR_FAIL(
        auto oriented,
        service->ensureImageVariant(1, original.record.id, {8, 8, 80, "png"}));
    EXPECT_EQ(oriented.width, 4);
    EXPECT_EQ(oriented.height, 8);
    Magick::Image decoded(path(oriented.file.hash).string());
    EXPECT_GT(decoded.exifProfile().length(), 0);
    EXPECT_EQ(decoded.orientation(), Magick::TopLeftOrientation);

    std::list<Magick::Image> frames;
    frames.emplace_back(Magick::Geometry(4, 4), Magick::Color("red"));
    frames.emplace_back(Magick::Geometry(4, 4), Magick::Color("blue"));
    for(auto& frame : frames)
    {
        frame.magick("WEBP");
        frame.animationDelay(10);
    }
    Magick::writeImages(frames.begin(), frames.end(), &blob, true);
    ASSIGN_OR_FAIL(auto animated, upload(1, "animated",
                                         {static_cast<const char*>(blob.data()),
                                          blob.length()}));
    EXPECT_FALSE(
        service->ensureImageVariant(1, animated.record.id, {8, 8, 80, "png"}));
    EXPECT_TRUE(std::filesystem::exists(path(animated.record.file.hash)));
}

TEST_F(Attachments, DeleteDuringUploadPublishesNewReference)
{
    ASSIGN_OR_FAIL(auto first, upload(1, "file", "abc"));
    ASSIGN_OR_FAIL(auto writer, service->beginUpload(1, "file"));
    ASSERT_TRUE(writer.append("abc"));
    ASSERT_TRUE(service->deleteAttachment(1, first.record.id));
    ASSIGN_OR_FAIL(auto second, writer.finish());
    EXPECT_NE(first.record.id, second.record.id);
    EXPECT_EQ(bytes(second.record.file.hash), "abc");
}

TEST_F(Attachments, RestartRemovesAbandonedStagingAndResumesCleanup)
{
    ASSIGN_OR_FAIL(auto result, upload(1, "file", "abc"));
    ASSERT_TRUE(inspection->execute("DELETE FROM Attachments"));
    ASSIGN_OR_FAIL(auto sql, inspection->statementFromStr(
                                 "INSERT INTO AttachmentCleanup VALUES(?)"));
    ASSERT_TRUE(sql.bind(result.record.file.hash));
    ASSERT_TRUE(inspection->execute(std::move(sql)));
    {
        std::ofstream abandoned(service->directory() / ".staging" /
                                "interrupted");
        abandoned << "partial upload";
    }
    {
        std::ofstream unknown(service->directory() / "operator_file");
        unknown << "do not delete unknown entries";
    }
    restart();
    EXPECT_TRUE(std::filesystem::is_empty(service->directory() / ".staging"));
    EXPECT_FALSE(std::filesystem::exists(path(result.record.file.hash)));
    EXPECT_TRUE(
        std::filesystem::exists(service->directory() / "operator_file"));
}

TEST_F(Attachments, SharedVariantOutputsSurviveUntilFinalSourceDeletion)
{
    ASSIGN_OR_FAIL(auto first, upload(1, "source", image()));
    ASSIGN_OR_FAIL(auto second, upload(2, "source", image("JPEG")));
    // Explicit mapping isolates reference accounting from encoder differences.
    ASSIGN_OR_FAIL(auto output, upload(1, "result", "generated"));
    for(const auto& source : {first.record.file.hash, second.record.file.hash})
    {
        ASSIGN_OR_FAIL(auto sql, inspection->statementFromStr(
                                     "INSERT INTO AttachmentVariants "
                                     "VALUES(?,'shared',?,1,1,'png',0)"));
        ASSERT_TRUE(sql.bind(source, output.record.file.hash));
        ASSERT_TRUE(inspection->execute(std::move(sql)));
    }
    ASSERT_TRUE(service->deleteAttachment(1, output.record.id));
    ASSERT_TRUE(service->deleteAttachment(1, first.record.id));
    EXPECT_TRUE(std::filesystem::exists(path(output.record.file.hash)));
    ASSERT_TRUE(service->deleteAttachment(2, second.record.id));
    EXPECT_FALSE(std::filesystem::exists(path(output.record.file.hash)));
}

TEST_F(Attachments, CommitFailureRollsBackMetadataAfterPublication)
{
    ASSERT_TRUE(inspection->execute(
        "CREATE TABLE DeferredFailure(id INTEGER REFERENCES Users(id) "
        "DEFERRABLE INITIALLY DEFERRED)"));
    ASSERT_TRUE(inspection->execute(R"(
CREATE TRIGGER fail_commit AFTER INSERT ON Attachments
BEGIN INSERT INTO DeferredFailure VALUES(999); END;
)"));
    EXPECT_FALSE(upload(1, "failed", "abc"));
    EXPECT_TRUE(service->listAttachments(1)->empty());
    ASSIGN_OR_FAIL(auto files, inspection->eval<int>(
                                   "SELECT count(*) FROM AttachmentFiles"));
    EXPECT_EQ(std::get<0>(files[0]), 0);
    EXPECT_FALSE(
        std::filesystem::exists(path("ba7816bf8f01cfea414140de5dae2223")));
    restart();
    EXPECT_FALSE(
        std::filesystem::exists(path("ba7816bf8f01cfea414140de5dae2223")));
}

TEST_F(Attachments, NewUploadCancelsPendingCleanup)
{
    ASSIGN_OR_FAIL(auto uploaded, upload(1, "file", "abc"));
    ASSIGN_OR_FAIL(auto sql, inspection->statementFromStr(
                                 "INSERT INTO AttachmentCleanup VALUES(?)"));
    ASSERT_TRUE(sql.bind(uploaded.record.file.hash));
    ASSERT_TRUE(inspection->execute(std::move(sql)));
    ASSIGN_OR_FAIL(auto adopted, upload(2, "other", "abc"));
    ASSERT_TRUE(service->cleanup());
    EXPECT_EQ(bytes(adopted.record.file.hash), "abc");
    ASSIGN_OR_FAIL(auto pending, inspection->eval<int>(
                                     "SELECT count(*) FROM AttachmentCleanup"));
    EXPECT_EQ(std::get<0>(pending[0]), 0);
    EXPECT_EQ(probeMimeType(service->directory() / "absent"),
              "application/octet-stream");
}

TEST_F(Attachments, ConversionRacingDeletionNeverResurrectsReferences)
{
    for(int i = 0; i < 12; ++i)
    {
        ASSIGN_OR_FAIL(auto original, upload(1, "race", image()));
        std::promise<void> start;
        auto ready = start.get_future();
        auto generation = std::async(
            std::launch::async,
            [this, id = original.record.id, ready = std::move(ready)]() mutable
            {
                ready.wait();
                return service->ensureImageVariant(1, id, {8, 8, 80, "webp"});
            });
        start.set_value();
        ASSERT_TRUE(service->deleteAttachment(1, original.record.id));
        auto result = generation.get();
        if(!result)
        {
            ASSERT_NE(result.error().as<HTTPError>(), nullptr);
            EXPECT_EQ(result.error().as<HTTPError>()->code, 404);
        }
        EXPECT_TRUE(service->listAttachments(1)->empty());
        ASSIGN_OR_FAIL(
            auto mappings,
            inspection->eval<int>("SELECT count(*) FROM AttachmentVariants"));
        EXPECT_EQ(std::get<0>(mappings[0]), 0);
        ASSIGN_OR_FAIL(auto files, inspection->eval<int>(
                                       "SELECT count(*) FROM AttachmentFiles"));
        EXPECT_EQ(std::get<0>(files[0]), 0);
    }
}

TEST_F(Attachments, LibmagicLoadFailureFallsBackWithoutAffectingStorage)
{
    struct Environment
    {
        std::optional<std::string> old;
        Environment()
        {
            if(const char* value = getenv("MAGIC")) { old = value; }
            setenv("MAGIC","/nonexistent/nsweekly_magic_database",1);
        }
        ~Environment()
        {
            if(old) { setenv("MAGIC",old->c_str(),1); }
            else { unsetenv("MAGIC"); }
        }
    } environment;
    ASSIGN_OR_FAIL(auto result,upload(1,"plain","abc"));
    EXPECT_EQ(result.record.file.mime_type,"application/octet-stream");
    EXPECT_EQ(bytes(result.record.file.hash),"abc");
}

namespace
{
// Records consumer hook ordering and allows storage failures at real
// boundaries.
class ConsumerUpload : public Upload
{
public:
    // Captures test observations that outlive this owned task.
    ConsumerUpload(UploadInput input, std::vector<std::string>& calls,
                   bool& destroyed,
                   std::function<E<void>(UploadProcessing&)> processing = {},
                   std::function<E<void>(UploadPublication&)> publication = {},
                   bool reject_start = false)
        : Upload(std::move(input)), calls(calls), destroyed(destroyed),
          processing(std::move(processing)),
          publication(std::move(publication)), reject_start(reject_start)
    {
    }
    // Makes lifetime release observable on both success and failure.
    ~ConsumerUpload() override
    {
        destroyed = true;
    }
    // Rejects before reception when requested.
    E<void> beginUpload(UploadStart&) override
    {
        calls.push_back("begin");
        if(reject_start)
        {
            return std::unexpected(runtimeError("Injected start failure"));
        }
        return {};
    }
    // Invokes a consumer-specific processing action after reception.
    E<void> process(UploadProcessing& context) override
    {
        calls.push_back("process");
        return processing ? processing(context) : E<void>{};
    }
    // Writes or rejects within the shared publication transaction.
    E<void> endUpload(UploadPublication& context) override
    {
        calls.push_back("end");
        return publication ? publication(context) : E<void>{};
    }

private:
    std::vector<std::string>& calls;
    bool& destroyed;
    std::function<E<void>(UploadProcessing&)> processing;
    std::function<E<void>(UploadPublication&)> publication;
    bool reject_start;
};

// Drives synchronous reception in bounded chunks without retaining the file.
struct UploadChunks
{
    size_t offset = 0;
    E<std::string> operator()()
    {
        const std::string bytes = "abcdef";
        auto chunk = bytes.substr(offset, 2);
        offset += chunk.size();
        return chunk;
    }
};
} // namespace

TEST_F(Attachments,
       ConsumerTaskUsesSameLifecycleForSynchronousAndStreamingInput)
{
    std::vector<std::string> calls;
    bool destroyed = false;
    auto task = std::make_unique<ConsumerUpload>(
        UploadInput{1, "sync.txt", UploadChunks{}}, calls, destroyed);
    ASSIGN_OR_FAIL(auto result, service->doUpload(std::move(task)));
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(calls, (std::vector<std::string>{"begin", "process", "end"}));
    EXPECT_EQ(bytes(result.record.file.hash), "abcdef");
    calls.clear();
    destroyed = false;
    auto streamed = std::make_unique<ConsumerUpload>(UploadInput{1, "sync.txt"},
                                                     calls, destroyed);
    ASSIGN_OR_FAIL(auto handle, service->startUpload(std::move(streamed)));
    ASSERT_TRUE(handle->append("abcdef"));
    EXPECT_EQ(calls, (std::vector<std::string>{"begin"}));
    EXPECT_FALSE(destroyed);
    ASSIGN_OR_FAIL(auto duplicate, handle->finish());
    EXPECT_EQ(duplicate.record.id, result.record.id);
    EXPECT_TRUE(duplicate.already_owned);
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(calls, (std::vector<std::string>{"begin", "process", "end"}));
    EXPECT_FALSE(handle->finish());
    EXPECT_FALSE(handle->append("more"));
}

TEST_F(Attachments, HookFailuresStopExecutionAndReleaseOwnedTask)
{
    std::vector<std::string> calls;
    bool destroyed = false;
    auto rejected = std::make_unique<ConsumerUpload>(
        UploadInput{1, "reject"}, calls, destroyed, nullptr, nullptr, true);
    EXPECT_FALSE(service->startUpload(std::move(rejected)));
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(calls, (std::vector<std::string>{"begin"}));
    calls.clear();
    destroyed = false;
    auto task = std::make_unique<ConsumerUpload>(
        UploadInput{1, "process"}, calls, destroyed,
        [](UploadProcessing&) -> E<void>
        {
            return std::unexpected(runtimeError("Injected processing failure"));
        });
    ASSIGN_OR_FAIL(auto handle, service->startUpload(std::move(task)));
    ASSERT_TRUE(handle->append("bytes"));
    EXPECT_FALSE(handle->finish());
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(calls, (std::vector<std::string>{"begin", "process"}));
    EXPECT_TRUE(service->listAttachments(1)->empty());
    EXPECT_TRUE(std::filesystem::is_empty(service->directory() / ".staging"));
}

TEST_F(Attachments, ConsumerWritesRollBackWithAttachmentOnEndHookFailure)
{
    ASSERT_TRUE(inspection->execute(
        "CREATE TABLE ConsumerAssociations(attachment_id INTEGER "
        "REFERENCES Attachments(id))"));
    std::vector<std::string> calls;
    bool destroyed = false;
    std::string source_hash;
    auto task = std::make_unique<ConsumerUpload>(
        UploadInput{1, "rollback"}, calls, destroyed, nullptr,
        [&](UploadPublication& context) -> E<void>
        {
            source_hash = context.attachment.record.file.hash;
            ASSIGN_OR_RETURN(auto sql,
                             context.database.statementFromStr(
                                 "INSERT INTO ConsumerAssociations VALUES(?)"));
            DO_OR_RETURN(sql.bind(context.attachment.record.id));
            DO_OR_RETURN(context.database.execute(std::move(sql)));
            // A separate reader cannot see either uncommitted module's rows.
            EXPECT_EQ(std::get<0>(inspection
                                      ->eval<int64_t>(
                                          "SELECT count(*) FROM Attachments")
                                      ->at(0)),
                      0);
            EXPECT_EQ(
                std::get<0>(inspection
                                ->eval<int64_t>(
                                    "SELECT count(*) FROM ConsumerAssociations")
                                ->at(0)),
                0);
            return std::unexpected(runtimeError("Injected consumer failure"));
        });
    ASSIGN_OR_FAIL(auto handle, service->startUpload(std::move(task)));
    ASSERT_TRUE(handle->append("original bytes"));
    EXPECT_FALSE(handle->finish());
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(std::get<0>(inspection
                              ->eval<int64_t>(
                                  "SELECT count(*) FROM ConsumerAssociations")
                              ->at(0)),
              0);
    EXPECT_TRUE(service->listAttachments(1)->empty());
    EXPECT_FALSE(std::filesystem::exists(path(source_hash)));
    EXPECT_TRUE(std::filesystem::is_empty(service->directory() / ".staging"));
}

TEST_F(Attachments, CommitFailureRollsBackBothModulesAndPublishedFiles)
{
    ASSERT_TRUE(inspection->execute(
        "CREATE TABLE ConsumerAssociations(attachment_id INTEGER "
        "REFERENCES Attachments(id), user_id INTEGER REFERENCES Users(id) "
        "DEFERRABLE INITIALLY DEFERRED)"));
    std::vector<std::string> calls;
    bool destroyed = false;
    std::string source_hash;
    auto task = std::make_unique<ConsumerUpload>(
        UploadInput{1, "commit"}, calls, destroyed, nullptr,
        [&](UploadPublication& context) -> E<void>
        {
            source_hash = context.attachment.record.file.hash;
            ASSIGN_OR_RETURN(
                auto sql,
                context.database.statementFromStr(
                    "INSERT INTO ConsumerAssociations VALUES(?,999)"));
            DO_OR_RETURN(sql.bind(context.attachment.record.id));
            return context.database.execute(std::move(sql));
        });
    ASSIGN_OR_FAIL(auto handle, service->startUpload(std::move(task)));
    ASSERT_TRUE(handle->append("bytes"));
    EXPECT_FALSE(handle->finish());
    EXPECT_TRUE(destroyed);
    EXPECT_TRUE(service->listAttachments(1)->empty());
    EXPECT_EQ(std::get<0>(inspection
                              ->eval<int64_t>(
                                  "SELECT count(*) FROM ConsumerAssociations")
                              ->at(0)),
              0);
    EXPECT_FALSE(std::filesystem::exists(path(source_hash)));
    EXPECT_TRUE(upload(1, "connection usable", "bytes"));
}

TEST_F(Attachments, ProcessingSnapshotCannotResurrectDeletedAttachment)
{
    ASSIGN_OR_FAIL(auto original, upload(1, "existing", "bytes"));
    std::vector<std::string> calls;
    bool destroyed = false;
    auto task = std::make_unique<ConsumerUpload>(
        UploadInput{1, "existing"}, calls, destroyed,
        [&](UploadProcessing& context) -> E<void>
        {
            EXPECT_TRUE(context.existingAttachment());
            return service->deleteAttachment(1, original.record.id);
        });
    ASSIGN_OR_FAIL(auto handle, service->startUpload(std::move(task)));
    ASSERT_TRUE(handle->append("bytes"));
    auto failed = handle->finish();
    ASSERT_FALSE(failed);
    ASSERT_NE(failed.error().as<HTTPError>(), nullptr);
    EXPECT_EQ(failed.error().as<HTTPError>()->code, 409);
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(calls, (std::vector<std::string>{"begin", "process"}));
    EXPECT_TRUE(service->listAttachments(1)->empty());
}

TEST_F(Attachments, GenericManifestRecoveryPreservesCommittedAndActiveFiles)
{
    ASSIGN_OR_FAIL(auto original, upload(1, "kept", "bytes"));
    ASSIGN_OR_FAIL(auto orphan,
                   AttachmentStaging::create(service->directory()));
    ASSERT_TRUE(orphan->append("orphan bytes"));
    ASSIGN_OR_FAIL(auto file, orphan->finish());
    ASSERT_TRUE(publishAttachment(service->directory(), orphan->path(), file));
    auto manifest = service->directory() / ".staging" / "recovery.manifest";
    {
        std::ofstream output(manifest);
        output << file.hash << '\n' << original.record.file.hash << '\n';
    }
    ASSIGN_OR_FAIL(auto active, service->beginUpload(1, "active"));
    ASSERT_TRUE(active.append("active bytes"));
    ASSERT_TRUE(service->cleanup());
    EXPECT_FALSE(std::filesystem::exists(path(file.hash)));
    EXPECT_FALSE(std::filesystem::exists(manifest));
    EXPECT_EQ(bytes(original.record.file.hash), "bytes");
    EXPECT_TRUE(active.finish());
}

TEST_F(Attachments, RejectsNestedConsumerTransactionsAndConsumedInput)
{
    OrdinaryUpload task({1, "once"});
    ASSERT_TRUE(task.takeInput());
    EXPECT_FALSE(task.takeInput());
    EXPECT_FALSE(service->writeMetadata(
        [](UploadDatabase& db) { return db.execute("BEGIN IMMEDIATE"); }));
    EXPECT_FALSE(service->writeMetadata(
        [](UploadDatabase& db)
        { return db.execute("-- consumer comment\n /* comment */ COMMIT;"); }));
    EXPECT_TRUE(upload(1, "usable", "bytes"));
}

TEST_F(Attachments, SourceErrorsCancelBeforeProcessingAndPublication)
{
    std::vector<std::string> calls;
    bool destroyed = false;
    int reads = 0;
    UploadInput input{1, "interrupted", [&]() -> E<std::string>
                      {
                          if(reads++ == 0)
                          {
                              return "first chunk";
                          }
                          return std::unexpected(
                              runtimeError("Interrupted input"));
                      }};
    EXPECT_FALSE(service->doUpload(
        std::make_unique<ConsumerUpload>(std::move(input), calls, destroyed)));
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(calls, (std::vector<std::string>{"begin"}));
    EXPECT_TRUE(service->listAttachments(1)->empty());
    EXPECT_TRUE(std::filesystem::is_empty(service->directory() / ".staging"));
}

TEST_F(Attachments, RollbackPreservesBytesOwnedByAnotherAttachment)
{
    ASSIGN_OR_FAIL(auto kept, upload(2, "shared", "bytes"));
    std::vector<std::string> calls;
    bool destroyed = false;
    auto task = std::make_unique<ConsumerUpload>(
        UploadInput{1, "new"}, calls, destroyed, nullptr,
        [](UploadPublication&) -> E<void>
        {
            return std::unexpected(
                runtimeError("Consumer rejected association"));
        });
    ASSIGN_OR_FAIL(auto handle, service->startUpload(std::move(task)));
    ASSERT_TRUE(handle->append("bytes"));
    EXPECT_FALSE(handle->finish());
    EXPECT_TRUE(service->listAttachments(1)->empty());
    EXPECT_TRUE(service->getAttachment(2, kept.record.id));
    EXPECT_EQ(bytes(kept.record.file.hash), "bytes");
}

TEST_F(Attachments, FailedFileInstallationRetainsRecoveryUntilUnlinkSucceeds)
{
    std::vector<std::string> calls;
    bool destroyed = false;
    std::filesystem::path blocked;
    auto task = std::make_unique<ConsumerUpload>(
        UploadInput{1, "blocked"}, calls, destroyed,
        [&](UploadProcessing& context) -> E<void>
        {
            blocked = path(context.source().hash);
            std::filesystem::create_directories(blocked);
            std::ofstream(blocked / "sentinel") << "cannot unlink directory";
            return {};
        });
    ASSIGN_OR_FAIL(auto handle, service->startUpload(std::move(task)));
    ASSERT_TRUE(handle->append("blocked bytes"));
    EXPECT_FALSE(handle->finish());
    EXPECT_TRUE(destroyed);
    EXPECT_TRUE(service->listAttachments(1)->empty());
    EXPECT_FALSE(std::filesystem::is_empty(service->directory() / ".staging"));
    std::filesystem::remove_all(blocked);
    ASSERT_TRUE(service->cleanup());
    EXPECT_TRUE(std::filesystem::is_empty(service->directory() / ".staging"));
}

TEST_F(Attachments,
       RestartRecoversUncommittedManifestAndPreservesCommittedHashes)
{
    ASSIGN_OR_FAIL(auto kept, upload(1, "kept", "committed bytes"));
    ASSIGN_OR_FAIL(auto staging,
                   AttachmentStaging::create(service->directory()));
    ASSERT_TRUE(staging->append("uncommitted bytes"));
    ASSIGN_OR_FAIL(auto orphan, staging->finish());
    ASSERT_TRUE(
        publishAttachment(service->directory(), staging->path(), orphan));
    staging.reset();
    auto manifest = service->directory() / ".staging" / "crashed.manifest";
    {
        std::ofstream output(manifest);
        output << orphan.hash << '\n' << kept.record.file.hash << '\n';
    }
    restart();
    EXPECT_TRUE(service->getAttachment(1, kept.record.id));
    EXPECT_EQ(bytes(kept.record.file.hash), "committed bytes");
    EXPECT_FALSE(std::filesystem::exists(path(orphan.hash)));
    EXPECT_TRUE(std::filesystem::is_empty(service->directory() / ".staging"));
}

TEST_F(Attachments, CachedProcessingSnapshotCannotPublishAfterVariantRemoval)
{
    auto pixels = image();
    ASSIGN_OR_FAIL(auto original, upload(2, "source.png", pixels));
    ImageVariantOptions options{8, 8, 80, "webp"};
    ASSIGN_OR_FAIL(auto variant,
                   service->ensureImageVariant(2, original.record.id, options));
    std::vector<std::string> calls;
    bool destroyed = false;
    auto task = std::make_unique<ConsumerUpload>(
        UploadInput{1, "new.png"}, calls, destroyed,
        [&](UploadProcessing& context) -> E<void>
        {
            DO_OR_RETURN(context.prepareVariant(options));
            return service->deleteAttachment(2, original.record.id);
        });
    ASSIGN_OR_FAIL(auto handle, service->startUpload(std::move(task)));
    ASSERT_TRUE(handle->append(pixels));
    auto result = handle->finish();
    ASSERT_FALSE(result);
    ASSERT_NE(result.error().as<HTTPError>(), nullptr);
    EXPECT_EQ(result.error().as<HTTPError>()->code, 409);
    EXPECT_TRUE(destroyed);
    EXPECT_TRUE(service->listAttachments(1)->empty());
    EXPECT_FALSE(std::filesystem::exists(path(variant.file.hash)));
}

TEST_F(Attachments, PublicationRejectsStatementsPreparedOnAnotherConnection)
{
    ASSERT_TRUE(inspection->execute(
        "CREATE TABLE ConsumerAssociations(attachment_id INTEGER)"));
    ASSIGN_OR_FAIL(auto foreign,
                   inspection->statementFromStr(
                       "INSERT INTO ConsumerAssociations VALUES(42)"));
    std::vector<std::string> calls;
    bool destroyed = false;
    auto task = std::make_unique<ConsumerUpload>(
        UploadInput{1, "foreign"}, calls, destroyed, nullptr,
        [&](UploadPublication& context)
        { return context.database.execute(std::move(foreign)); });
    ASSIGN_OR_FAIL(auto handle, service->startUpload(std::move(task)));
    ASSERT_TRUE(handle->append("bytes"));
    EXPECT_FALSE(handle->finish());
    EXPECT_TRUE(destroyed);
    EXPECT_TRUE(service->listAttachments(1)->empty());
    EXPECT_EQ(std::get<0>(inspection
                              ->eval<int64_t>(
                                  "SELECT count(*) FROM ConsumerAssociations")
                              ->at(0)),
              0);
}
