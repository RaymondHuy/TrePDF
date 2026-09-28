#pragma once

#include "ApplicationFonts.h"

#include <QVector>

#include <memory>

namespace WindowsSystemFonts {

// A physical installed face. DirectWrite handles stay private so this helper's
// consumers do not inherit Windows headers or COM ownership requirements.
class Face
{
public:
    static Face resolve(const QString &family, bool bold, bool italic);

    bool isValid() const;
    QVector<quint32> tableTags(ApplicationFonts::FontFaceData::Error &error) const;
    // Tags use sfnt's big-endian order, matching CoreText and the font writer.
    QByteArray table(quint32 bigEndianTag) const;
    bool supportsCharacters(const QVector<uint> &characters) const;

private:
    struct Data;
    std::shared_ptr<Data> m_data;
};

} // namespace WindowsSystemFonts
