// SPDX-License-Identifier: Apache-2.0
#include "User/Toolkit/Fixture.hpp"
#include "User/Imaging/Imaging.hpp"
namespace Asteria::Test
{
namespace
{
void Adapter(TestSupport::TestContext& test) noexcept
{
    ToolkitFixture::Host host;
    User::Toolkit::Application app{host, host, User::Toolkit::Theme::Classic()};
    User::Toolkit::PictureBox box{app};
    const Foundation::UInt8 pixels[]{255U, 0U, 0U, 255U};
    const User::Imaging::Image image{pixels, sizeof(pixels), 1U, 1U};
    ASTERIA_EXPECT(test, "decoded RGBA borrowed by PictureBox",
                   box.SetDecodedImage(image) == User::Toolkit::Status::Ok);
    ASTERIA_EXPECT(test, "invalid decoded image refused",
                   box.SetDecodedImage({pixels, 3U, 1U, 1U}) == User::Toolkit::Status::Invalid);
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"Toolkit imaging adapter"};
    Asteria::Test::Adapter(test);
    return test.Finish();
}
