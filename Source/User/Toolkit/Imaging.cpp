// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/Controls.hpp"
#include "User/Imaging/Imaging.hpp"
namespace Asteria::User::Toolkit
{
Status PictureBox::SetDecodedImage(const Imaging::Image& image, bool scale) noexcept
{
    // Output stays caller-owned.
    if (!image.Valid())
    {
        return Status::Invalid;
    }
    return SetImage({image.Pixels, image.Bytes, image.Width, image.Height, image.Width * 4U, ImageFormat::Rgba}, scale);
}
} // Namespace Asteria::User::Toolkit
