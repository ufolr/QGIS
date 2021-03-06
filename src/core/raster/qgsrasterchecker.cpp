/***************************************************************************
     qgsrasterchecker.cpp
     --------------------------------------
    Date                 : 5 Sep 2012
    Copyright            : (C) 2012 by Radim Blazek
    Email                : radim dot blazek at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsproviderregistry.h"
#include "qgsrasterchecker.h"
#include "qgsrasterdataprovider.h"
#include "qgsrasterlayer.h"

#include <QColor>
#include <QPainter>
#include <QImage>
#include <QTime>
#include <QCryptographicHash>
#include <QByteArray>
#include <QDebug>
#include <QBuffer>

QgsRasterChecker::QgsRasterChecker()
{
  mTabStyle = QStringLiteral( "border-spacing: 0px; border-width: 1px 1px 0 0; border-style: solid;" );
  mCellStyle = QStringLiteral( "border-width: 0 0 1px 1px; border-style: solid; font-size: smaller; text-align: center;" );
  mOkStyle = QStringLiteral( "background: #00ff00;" );
  mErrStyle = QStringLiteral( "background: #ff0000;" );
  mErrMsgStyle = QStringLiteral( "color: #ff0000;" );
}

bool QgsRasterChecker::runTest( const QString &verifiedKey, QString verifiedUri,
                                const QString &expectedKey, QString expectedUri )
{
  bool ok = true;
  mReport += QLatin1String( "\n\n" );

  //QgsRasterDataProvider* verifiedProvider = QgsRasterLayer::loadProvider( verifiedKey, verifiedUri );
  QgsDataProvider::ProviderOptions options;
  QgsRasterDataProvider *verifiedProvider = dynamic_cast< QgsRasterDataProvider * >( QgsProviderRegistry::instance()->createProvider( verifiedKey, verifiedUri, options ) );
  if ( !verifiedProvider || !verifiedProvider->isValid() )
  {
    error( QStringLiteral( "Cannot load provider %1 with URI: %2" ).arg( verifiedKey, verifiedUri ), mReport );
    ok = false;
  }

  //QgsRasterDataProvider* expectedProvider = QgsRasterLayer::loadProvider( expectedKey, expectedUri );
  QgsRasterDataProvider *expectedProvider = dynamic_cast< QgsRasterDataProvider * >( QgsProviderRegistry::instance()->createProvider( expectedKey, expectedUri, options ) );
  if ( !expectedProvider || !expectedProvider->isValid() )
  {
    error( QStringLiteral( "Cannot load provider %1 with URI: %2" ).arg( expectedKey, expectedUri ), mReport );
    ok = false;
  }

  if ( !ok ) return false;

  mReport += QStringLiteral( "Verified URI: %1<br>" ).arg( verifiedUri.replace( '&', QLatin1String( "&amp;" ) ) );
  mReport += QStringLiteral( "Expected URI: %1<br>" ).arg( expectedUri.replace( '&', QLatin1String( "&amp;" ) ) );

  mReport += QLatin1String( "<br>" );
  mReport += QStringLiteral( "<table style='%1'>\n" ).arg( mTabStyle );
  mReport += compareHead();

  compare( QStringLiteral( "Band count" ), verifiedProvider->bandCount(), expectedProvider->bandCount(), mReport, ok );

  compare( QStringLiteral( "Width" ), verifiedProvider->xSize(), expectedProvider->xSize(), mReport, ok );
  compare( QStringLiteral( "Height" ), verifiedProvider->ySize(), expectedProvider->ySize(), mReport, ok );

  compareRow( QStringLiteral( "Extent" ), verifiedProvider->extent().toString(), expectedProvider->extent().toString(), mReport, verifiedProvider->extent() == expectedProvider->extent() );

  if ( verifiedProvider->extent() != expectedProvider->extent() ) ok = false;


  mReport += QLatin1String( "</table>\n" );

  if ( !ok ) return false;

  bool allOk = true;
  for ( int band = 1; band <= expectedProvider->bandCount(); band++ )
  {
    mReport += QStringLiteral( "<h3>Band %1</h3>\n" ).arg( band );
    mReport += QStringLiteral( "<table style='%1'>\n" ).arg( mTabStyle );
    mReport += compareHead();

    // Data types may differ (?)
    bool typesOk = true;
    compare( QStringLiteral( "Source data type" ), verifiedProvider->sourceDataType( band ), expectedProvider->sourceDataType( band ), mReport, typesOk );
    compare( QStringLiteral( "Data type" ), verifiedProvider->dataType( band ), expectedProvider->dataType( band ), mReport, typesOk );

    // Check nodata
    bool noDataOk = true;
    compare( QStringLiteral( "No data (NULL) value existence flag" ), verifiedProvider->sourceHasNoDataValue( band ), expectedProvider->sourceHasNoDataValue( band ), mReport, noDataOk );
    if ( verifiedProvider->sourceHasNoDataValue( band ) && expectedProvider->sourceHasNoDataValue( band ) )
    {
      compare( QStringLiteral( "No data (NULL) value" ), verifiedProvider->sourceNoDataValue( band ), expectedProvider->sourceNoDataValue( band ), mReport, noDataOk );
    }

    bool statsOk = true;
    QgsRasterBandStats verifiedStats = verifiedProvider->bandStatistics( band );
    QgsRasterBandStats expectedStats = expectedProvider->bandStatistics( band );

    // Min/max may 'slightly' differ, for big numbers however, the difference may
    // be quite big, for example for Float32 with max -3.332e+38, the difference is 1.47338e+24
    double tol = tolerance( expectedStats.minimumValue );
    compare( QStringLiteral( "Minimum value" ), verifiedStats.minimumValue, expectedStats.minimumValue, mReport, statsOk, tol );
    tol = tolerance( expectedStats.maximumValue );
    compare( QStringLiteral( "Maximum value" ), verifiedStats.maximumValue, expectedStats.maximumValue, mReport, statsOk, tol );

    // TODO: enable once fixed (WCS excludes nulls but GDAL does not)
    //compare( "Cells count", verifiedStats.elementCount, expectedStats.elementCount, mReport, statsOk );

    tol = tolerance( expectedStats.mean );
    compare( QStringLiteral( "Mean" ), verifiedStats.mean, expectedStats.mean, mReport, statsOk, tol );

    // stdDev usually differ significantly
    tol = tolerance( expectedStats.stdDev, 1 );
    compare( QStringLiteral( "Standard deviation" ), verifiedStats.stdDev, expectedStats.stdDev, mReport, statsOk, tol );

    mReport += QLatin1String( "</table>" );
    mReport += QLatin1String( "<br>" );

    if ( !statsOk || !typesOk || !noDataOk )
    {
      allOk = false;
      // create values table anyway so that values are available
    }

    mReport += QLatin1String( "<table><tr>" );
    mReport += QLatin1String( "<td>Data comparison</td>" );
    mReport += QStringLiteral( "<td style='%1 %2 border: 1px solid'>correct&nbsp;value</td>" ).arg( mCellStyle, mOkStyle );
    mReport += QLatin1String( "<td></td>" );
    mReport += QStringLiteral( "<td style='%1 %2 border: 1px solid'>wrong&nbsp;value<br>expected value</td></tr>" ).arg( mCellStyle, mErrStyle );
    mReport += QLatin1String( "</tr></table>" );
    mReport += QLatin1String( "<br>" );

    int width = expectedProvider->xSize();
    int height = expectedProvider->ySize();
    QgsRasterBlock *expectedBlock = expectedProvider->block( band, expectedProvider->extent(), width, height );
    QgsRasterBlock *verifiedBlock = verifiedProvider->block( band, expectedProvider->extent(), width, height );

    if ( !expectedBlock || !expectedBlock->isValid() ||
         !verifiedBlock || !verifiedBlock->isValid() )
    {
      allOk = false;
      mReport += QLatin1String( "cannot read raster block" );
      continue;
    }

    // compare data values
    QString htmlTable = QStringLiteral( "<table style='%1'>" ).arg( mTabStyle );
    for ( int row = 0; row < height; row ++ )
    {
      htmlTable += QLatin1String( "<tr>" );
      for ( int col = 0; col < width; col ++ )
      {
        bool cellOk = true;
        double verifiedVal = verifiedBlock->value( row, col );
        double expectedVal = expectedBlock->value( row, col );

        QString valStr;
        if ( compare( verifiedVal, expectedVal, 0 ) )
        {
          valStr = QStringLiteral( "%1" ).arg( verifiedVal );
        }
        else
        {
          cellOk = false;
          allOk = false;
          valStr = QStringLiteral( "%1<br>%2" ).arg( verifiedVal ).arg( expectedVal );
        }
        htmlTable += QStringLiteral( "<td style='%1 %2'>%3</td>" ).arg( mCellStyle, cellOk ? mOkStyle : mErrStyle, valStr );
      }
      htmlTable += QLatin1String( "</tr>" );
    }
    htmlTable += QLatin1String( "</table>" );

    mReport += htmlTable;

    delete expectedBlock;
    delete verifiedBlock;
  }
  delete verifiedProvider;
  delete expectedProvider;
  return allOk;
}

void QgsRasterChecker::error( const QString &message, QString &report )
{
  report += QStringLiteral( "<font style='%1'>Error: " ).arg( mErrMsgStyle );
  report += message;
  report += QLatin1String( "</font>" );
}

double QgsRasterChecker::tolerance( double val, int places )
{
  // float precision is about 7 decimal digits, double about 16
  // default places = 6
  return 1. * std::pow( 10, std::round( std::log10( std::fabs( val ) ) - places ) );
}

QString QgsRasterChecker::compareHead()
{
  QString html;
  html += QStringLiteral( "<tr><th style='%1'>Param name</th><th style='%1'>Verified value</th><th style='%1'>Expected value</th><th style='%1'>Difference</th><th style='%1'>Tolerance</th></tr>" ).arg( mCellStyle );
  return html;
}

void QgsRasterChecker::compare( const QString &paramName, int verifiedVal, int expectedVal, QString &report, bool &ok )
{
  bool isEqual = verifiedVal == expectedVal;
  compareRow( paramName, QString::number( verifiedVal ), QString::number( expectedVal ), report, isEqual, QString::number( verifiedVal - expectedVal ) );
  if ( !isEqual )
    ok = false;
}

bool QgsRasterChecker::compare( double verifiedVal, double expectedVal, double tolerance )
{
  // values may be nan
  return ( std::isnan( verifiedVal ) && std::isnan( expectedVal ) ) || ( std::fabs( verifiedVal - expectedVal ) <= tolerance );
}

void QgsRasterChecker::compare( const QString &paramName, double verifiedVal, double expectedVal, QString &report, bool &ok, double tolerance )
{
  bool isNearEqual = compare( verifiedVal, expectedVal, tolerance );
  compareRow( paramName, QString::number( verifiedVal ), QString::number( expectedVal ), report, isNearEqual, QString::number( verifiedVal - expectedVal ), QString::number( tolerance ) );
  if ( !isNearEqual )
    ok = false;
}

void QgsRasterChecker::compareRow( const QString &paramName, const QString &verifiedVal, const QString &expectedVal, QString &report, bool ok, const QString &difference, const QString &tolerance )
{
  report += QLatin1String( "<tr>\n" );
  report += QStringLiteral( "<td style='%1'>%2</td><td style='%1 %3'>%4</td><td style='%1'>%5</td>\n" ).arg( mCellStyle, paramName, ok ? mOkStyle : mErrStyle, verifiedVal, expectedVal );
  report += QStringLiteral( "<td style='%1'>%2</td>\n" ).arg( mCellStyle, difference );
  report += QStringLiteral( "<td style='%1'>%2</td>\n" ).arg( mCellStyle, tolerance );
  report += QLatin1String( "</tr>" );
}
