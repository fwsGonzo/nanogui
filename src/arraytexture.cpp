/*
    nanogui/ArrayTexture.cpp -- Widget used to display images.

    The image view widget was contributed by Stefan Ivanov.

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/arraytexture.h>
#include <nanogui/window.h>
#include <nanogui/screen.h>
#include <nanogui/theme.h>
#include <cmath>

NAMESPACE_BEGIN(nanogui)

namespace {
    std::vector<std::string> tokenize(const std::string &string,
                                      const std::string &delim = "\n",
                                      bool includeEmpty = false) {
        std::string::size_type lastPos = 0, pos = string.find_first_of(delim, lastPos);
        std::vector<std::string> tokens;

        while (lastPos != std::string::npos) {
            std::string substr = string.substr(lastPos, pos - lastPos);
            if (!substr.empty() || includeEmpty)
                tokens.push_back(std::move(substr));
            lastPos = pos;
            if (lastPos != std::string::npos) {
                lastPos += 1;
                pos = string.find_first_of(delim, lastPos);
            }
        }

        return tokens;
    }

    constexpr char const *const defaultArrayVertexShader =
        R"(#version 330
        uniform vec2 scaleFactor;
        uniform vec2 position;
        in vec2 vertex;
        in vec3 texCoords;
        out vec3 uv;
        void main() {
            uv = texCoords;
            vec2 scaledVertex = (vertex * scaleFactor) + position;
            gl_Position  = vec4(2.0*scaledVertex.x - 1.0,
                                1.0 - 2.0*scaledVertex.y,
                                0.0, 1.0);
        })";

    constexpr char const *const defaultArrayFragmentShader =
        R"(#version 330
        uniform sampler2DArray image;
        uniform float tile_id;
        out vec4 color;
        in vec3 uv;
        void main() {
            color = texture(image, uv);
        })";
}

static const float borderPx = 4.0f;
static const float deltaX = 8.0f;
static const float deltaY = 8.0f;

ArrayTexture::ArrayTexture(Widget* parent, const int tw, const int th, GLuint ttex)
    : Widget(parent), mImageID(ttex), tiles_x(tw), tiles(tw * th),
      mScale(1.0f), mOffset(Vector2f::Zero()),
      mFixedScale(false), mFixedOffset(false), mPixelInfoCallback(nullptr)
{
    assert(tw > 0 && th > 0);
    // widget size
    updateImageParameters();
    mShader.init("ArrayShader",
                 defaultArrayVertexShader,
                 defaultArrayFragmentShader);

    // generate vertices
    MatrixXf vertices(2, 4 * tiles.size());
    int i = 0;
    for (int y = 0; y < th; y++)
    for (int x = 0; x < tw; x++)
    {
      const float w = tile_w / (float) mImageSize.x();
      const float h = tile_h / (float) mImageSize.y();
      const float dx = (borderPx + (tile_w + deltaX) * x) / (float) mImageSize.x();
      const float dy = (borderPx + (tile_h + deltaY) * y) / (float) mImageSize.y();
      vertices.col(i*4 + 0) << dx+0, dy+0;
      vertices.col(i*4 + 1) << dx+w, dy+0;
      vertices.col(i*4 + 2) << dx+w, dy+h;
      vertices.col(i*4 + 3) << dx+0, dy+h;
      i++;
    }

    // dummy texcoords
    MatrixXf uvs(3, 4 * tiles.size());

    mShader.bind();
    mShader.uploadAttrib("vertex", vertices);
    mShader.uploadAttrib("texCoords", uvs);
}
ArrayTexture::ArrayTexture(Widget* parent, GLuint ttex)
  : ArrayTexture(parent, 1, 1, ttex)  {}

ArrayTexture::~ArrayTexture() {
    mShader.free();
}

void ArrayTexture::bindImage(GLuint imageId) {
    mImageID = imageId;
    updateImageParameters();
    fit();
}
void ArrayTexture::updateImageTexcoords()
{
  // tile-based texcoords
  MatrixXf uvs(3, 4 * tiles.size());
  for (size_t i = 0; i < tiles.size(); i++)
  {
    uvs.col(i*4 + 0) << 0, 1, tiles[i];
    uvs.col(i*4 + 1) << 1, 1, tiles[i];
    uvs.col(i*4 + 2) << 1, 0, tiles[i];
    uvs.col(i*4 + 3) << 0, 0, tiles[i];
  }

  mShader.bind();
  mShader.uploadAttrib("texCoords", uvs);
}
void ArrayTexture::setTile(short id, int x, int y) {
    tiles.at(x + y * this->tiles_x) = id;
    this->updateImageTexcoords();
}
void ArrayTexture::setTiles(std::vector<short> vec) {
    vec.resize(tiles.size());
    tiles = std::move(vec);
    this->updateImageTexcoords();
}

Vector2f ArrayTexture::imageCoordinateAt(const Vector2f& position) const {
    auto imagePosition = position - mOffset;
    return imagePosition / mScale;
}

Vector2f ArrayTexture::clampedImageCoordinateAt(const Vector2f& position) const {
    auto imageCoordinate = imageCoordinateAt(position);
    return imageCoordinate.cwiseMax(Vector2f::Zero()).cwiseMin(imageSizeF());
}

Vector2f ArrayTexture::positionForCoordinate(const Vector2f& imageCoordinate) const {
    return mScale*imageCoordinate + mOffset;
}

void ArrayTexture::setImageCoordinateAt(const Vector2f& position, const Vector2f& imageCoordinate) {
    // Calculate where the new offset must be in order to satisfy the image position equation.
    // Round the floating point values to balance out the floating point to integer conversions.
    mOffset = position - (imageCoordinate * mScale);

    // Clamp offset so that the image remains near the screen.
    mOffset = mOffset.cwiseMin(sizeF()).cwiseMax(-scaledImageSizeF());
}

void ArrayTexture::center() {
    mOffset = (sizeF() - scaledImageSizeF()) / 2;
}

void ArrayTexture::fit() {
    // Calculate the appropriate scaling factor.
    mScale = (sizeF().cwiseQuotient(imageSizeF())).minCoeff();
    center();
}

void ArrayTexture::setScaleCentered(float scale) {
    auto centerPosition = sizeF() / 2;
    auto p = imageCoordinateAt(centerPosition);
    mScale = scale;
    setImageCoordinateAt(centerPosition, p);
}

void ArrayTexture::moveOffset(const Vector2f& delta) {
    // Apply the delta to the offset.
    mOffset += delta;

    // Prevent the image from going out of bounds.
    auto scaledSize = scaledImageSizeF();
    if (mOffset.x() + scaledSize.x() < 0)
        mOffset.x() = -scaledSize.x();
    if (mOffset.x() > sizeF().x())
        mOffset.x() = sizeF().x();
    if (mOffset.y() + scaledSize.y() < 0)
        mOffset.y() = -scaledSize.y();
    if (mOffset.y() > sizeF().y())
        mOffset.y() = sizeF().y();
}

void ArrayTexture::zoom(int amount, const Vector2f& focusPosition) {
    auto focusedCoordinate = imageCoordinateAt(focusPosition);
    float scaleFactor = std::pow(mZoomSensitivity, amount);
    mScale = std::max(0.01f, scaleFactor * mScale);
    setImageCoordinateAt(focusPosition, focusedCoordinate);
}

bool ArrayTexture::mouseDragEvent(const Vector2i& p, const Vector2i& rel, int button, int /*modifiers*/) {
    if ((button & (1 << GLFW_MOUSE_BUTTON_LEFT)) != 0 && !mFixedOffset) {
        setImageCoordinateAt((p + rel).cast<float>(), imageCoordinateAt(p.cast<float>()));
        return true;
    }
    return false;
}

bool ArrayTexture::gridVisible() const {
    return (mGridThreshold != -1) && (mScale > mGridThreshold);
}

bool ArrayTexture::pixelInfoVisible() const {
    return mPixelInfoCallback && (mPixelInfoThreshold != -1) && (mScale > mPixelInfoThreshold);
}

bool ArrayTexture::helpersVisible() const {
    return gridVisible() || pixelInfoVisible();
}

bool ArrayTexture::scrollEvent(const Vector2i& p, const Vector2f& rel) {
    if (mFixedScale)
        return false;
    float v = rel.y();
    if (std::abs(v) < 1)
        v = std::copysign(1.f, v);
    zoom(v, (p - position()).cast<float>());
    return true;
}

bool ArrayTexture::keyboardEvent(int key, int /*scancode*/, int action, int modifiers) {
    if (action) {
        switch (key) {
        case GLFW_KEY_LEFT:
            if (!mFixedOffset) {
                if (GLFW_MOD_CONTROL & modifiers)
                    moveOffset(Vector2f(30, 0));
                else
                    moveOffset(Vector2f(10, 0));
                return true;
            }
            break;
        case GLFW_KEY_RIGHT:
            if (!mFixedOffset) {
                if (GLFW_MOD_CONTROL & modifiers)
                    moveOffset(Vector2f(-30, 0));
                else
                    moveOffset(Vector2f(-10, 0));
                return true;
            }
            break;
        case GLFW_KEY_DOWN:
            if (!mFixedOffset) {
                if (GLFW_MOD_CONTROL & modifiers)
                    moveOffset(Vector2f(0, -30));
                else
                    moveOffset(Vector2f(0, -10));
                return true;
            }
            break;
        case GLFW_KEY_UP:
            if (!mFixedOffset) {
                if (GLFW_MOD_CONTROL & modifiers)
                    moveOffset(Vector2f(0, 30));
                else
                    moveOffset(Vector2f(0, 10));
                return true;
            }
            break;
        }
    }
    return false;
}

bool ArrayTexture::keyboardCharacterEvent(unsigned int codepoint) {
    switch (codepoint) {
    case '-':
        if (!mFixedScale) {
            zoom(-1, sizeF() / 2);
            return true;
        }
        break;
    case '+':
        if (!mFixedScale) {
            zoom(1, sizeF() / 2);
            return true;
        }
        break;
    case 'c':
        if (!mFixedOffset) {
            center();
            return true;
        }
        break;
    case 'f':
        if (!mFixedOffset && !mFixedScale) {
            fit();
            return true;
        }
        break;
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9':
        if (!mFixedScale) {
            setScaleCentered(1 << (codepoint - '1'));
            return true;
        }
        break;
    default:
        return false;
    }
    return false;
}

Vector2i ArrayTexture::preferredSize(NVGcontext* /*ctx*/) const {
    return mImageSize;
}

void ArrayTexture::performLayout(NVGcontext* ctx) {
    Widget::performLayout(ctx);
    center();
}

void ArrayTexture::draw(NVGcontext* ctx) {
    Widget::draw(ctx);
    nvgEndFrame(ctx); // Flush the NanoVG draw stack, not necessary to call nvgBeginFrame afterwards.

    // Calculate several variables that need to be send to OpenGL in order for the image to be
    // properly displayed inside the widget.
    const Screen* screen = dynamic_cast<const Screen*>(this->window()->parent());
    assert(screen);
    Vector2f screenSize = screen->size().cast<float>();
    Vector2f scaleFactor = mScale * imageSizeF().cwiseQuotient(screenSize);
    Vector2f positionInScreen = absolutePosition().cast<float>();
    //Vector2f positionAfterOffset = positionInScreen + mOffset;
    Vector2f imagePosition = positionInScreen.cwiseQuotient(screenSize);
    glEnable(GL_SCISSOR_TEST);
    float r = screen->pixelRatio();
    glScissor(positionInScreen.x() * r,
              (screenSize.y() - positionInScreen.y() - size().y()) * r,
              size().x() * r, size().y() * r);
    mShader.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, mImageID);
    mShader.setUniform("image", 0);
    mShader.setUniform("scaleFactor", scaleFactor);
    mShader.setUniform("position", imagePosition);
    mShader.drawArray(GL_QUADS, 0, 4 * tiles.size());
    glDisable(GL_SCISSOR_TEST);

    if (helpersVisible())
        drawHelpers(ctx);

    drawWidgetBorder(ctx);
}

void ArrayTexture::updateImageParameters() {
    // Query the width of the OpenGL texture.
    glBindTexture(GL_TEXTURE_2D_ARRAY, mImageID);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &this->tile_w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &this->tile_h);
    const int tiles_y = tiles.size() / tiles_x;
    const int sizeX = this->tile_w * tiles_x + deltaX * (tiles_x-1);
    const int sizeY = this->tile_h * tiles_y + deltaX * (tiles_y-1);
    this->mImageSize = Vector2i(2 * borderPx + sizeX, 2 * borderPx + sizeY);
    printf("Image size: %d, %d\n", mImageSize.x(), mImageSize.y());
}

void ArrayTexture::drawWidgetBorder(NVGcontext* ctx) const {
    nvgBeginPath(ctx);
    nvgStrokeWidth(ctx, 1);
    nvgRoundedRect(ctx, mPos.x() + 0.5f, mPos.y() + 0.5f, mSize.x() - 1,
                   mSize.y() - 1, 0);
    nvgStrokeColor(ctx, mTheme->mWindowPopup);
    nvgStroke(ctx);

    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, mPos.x() + 0.5f, mPos.y() + 0.5f, mSize.x() - 1,
                   mSize.y() - 1, mTheme->mButtonCornerRadius);
    nvgStrokeColor(ctx, mTheme->mBorderDark);
    nvgStroke(ctx);
}

void ArrayTexture::drawHelpers(NVGcontext* ctx) const {
    // We need to apply mPos after the transformation to account for the position of the widget
    // relative to the parent.
    Vector2f upperLeftCorner = positionForCoordinate(Vector2f::Zero()) + positionF();
    Vector2f lowerRightCorner = positionForCoordinate(imageSizeF()) + positionF();
    if (gridVisible())
        drawPixelGrid(ctx, upperLeftCorner, lowerRightCorner, mScale);
    if (pixelInfoVisible())
        drawPixelInfo(ctx, mScale);
}

void ArrayTexture::drawPixelGrid(NVGcontext* ctx, const Vector2f& upperLeftCorner,
                              const Vector2f& lowerRightCorner, float stride) {
    nvgBeginPath(ctx);

    // Draw the vertical grid lines
    float currentX = upperLeftCorner.x();
    while (currentX <= lowerRightCorner.x()) {
        nvgMoveTo(ctx, std::round(currentX), std::round(upperLeftCorner.y()));
        nvgLineTo(ctx, std::round(currentX), std::round(lowerRightCorner.y()));
        currentX += stride;
    }

    // Draw the horizontal grid lines
    float currentY = upperLeftCorner.y();
    while (currentY <= lowerRightCorner.y()) {
        nvgMoveTo(ctx, std::round(upperLeftCorner.x()), std::round(currentY));
        nvgLineTo(ctx, std::round(lowerRightCorner.x()), std::round(currentY));
        currentY += stride;
    }

    nvgStrokeWidth(ctx, 1.0f);
    nvgStrokeColor(ctx, Color(1.0f, 1.0f, 1.0f, 0.2f));
    nvgStroke(ctx);
}

void ArrayTexture::drawPixelInfo(NVGcontext* ctx, float stride) const {
    // Extract the image coordinates at the two corners of the widget.
    Vector2i topLeft = clampedImageCoordinateAt(Vector2f::Zero())
                           .unaryExpr([](float x) { return std::floor(x); })
                           .cast<int>();

    Vector2i bottomRight = clampedImageCoordinateAt(sizeF())
                               .unaryExpr([](float x) { return std::ceil(x); })
                               .cast<int>();

    // Extract the positions for where to draw the text.
    Vector2f currentCellPosition =
        (positionF() + positionForCoordinate(topLeft.cast<float>()));

    float xInitialPosition = currentCellPosition.x();
    int xInitialIndex = topLeft.x();

    // Properly scale the pixel information for the given stride.
    auto fontSize = stride * mFontScaleFactor;
    static constexpr float maxFontSize = 30.0f;
    fontSize = fontSize > maxFontSize ? maxFontSize : fontSize;
    nvgBeginPath(ctx);
    nvgFontSize(ctx, fontSize);
    nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    nvgFontFace(ctx, "sans");
    while (topLeft.y() != bottomRight.y()) {
        while (topLeft.x() != bottomRight.x()) {
            writePixelInfo(ctx, currentCellPosition, topLeft, stride, fontSize);
            currentCellPosition.x() += stride;
            ++topLeft.x();
        }
        currentCellPosition.x() = xInitialPosition;
        currentCellPosition.y() += stride;
        ++topLeft.y();
        topLeft.x() = xInitialIndex;
    }
}

void ArrayTexture::writePixelInfo(NVGcontext* ctx, const Vector2f& cellPosition,
                               const Vector2i& pixel, float stride, float fontSize) const {
    auto pixelData = mPixelInfoCallback(pixel);
    auto pixelDataRows = tokenize(pixelData.first);

    // If no data is provided for this pixel then simply return.
    if (pixelDataRows.empty())
        return;

    nvgFillColor(ctx, pixelData.second);
    float yOffset = (stride - fontSize * pixelDataRows.size()) / 2;
    for (size_t i = 0; i != pixelDataRows.size(); ++i) {
        nvgText(ctx, cellPosition.x() + stride / 2, cellPosition.y() + yOffset,
                pixelDataRows[i].data(), nullptr);
        yOffset += fontSize;
    }
}

NAMESPACE_END(nanogui)
