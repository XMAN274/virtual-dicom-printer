/*
 * Copyright (C) 2014 Irkutsk Diagnostic Center.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "product.h"
#include "printscp.h"
#include "storescp.h"

#include "transcyrillic.h"

#include <QCoreApplication>
#include <QDate>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QRect>
#include <QRegExp>
#include <QSettings>
#include <QStringList>
#if (QT_VERSION >= QT_VERSION_CHECK(5, 0, 0))
#include <QUrlQuery>
#endif
#include <QXmlStreamReader>

#include <locale.h> // Required for tesseract

#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcsequen.h>
#include <dcmtk/dcmdata/dcvrui.h>
#include <dcmtk/dcmpstat/dvpsdef.h>     /* for constants */
#include <dcmtk/dcmimgle/dcmimage.h>    /* for DicomImage */

static void dump(const char* desc, DcmItem *dataset)
{
    if (!dataset)
        return;

    std::stringstream ss;
    dataset->print(ss);
    qDebug() << desc << ss.str().c_str();
}

static bool isDatasetPresent(T_DIMSE_Message &msg)
{
    switch (msg.CommandField)
    {
    case DIMSE_C_STORE_RQ: return msg.msg.CStoreRQ.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_C_STORE_RSP: return msg.msg.CStoreRSP.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_C_GET_RQ: return msg.msg.CGetRQ.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_C_GET_RSP: return msg.msg.CGetRSP.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_C_FIND_RQ: return msg.msg.CFindRQ.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_C_FIND_RSP: return msg.msg.CFindRSP.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_C_MOVE_RQ: return msg.msg.CMoveRQ.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_C_MOVE_RSP: return msg.msg.CMoveRSP.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_C_ECHO_RQ: return msg.msg.CEchoRQ.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_C_ECHO_RSP: return msg.msg.CEchoRSP.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_C_CANCEL_RQ: return msg.msg.CCancelRQ.DataSetType != DIMSE_DATASET_NULL;
    /* there is no DIMSE_C_CANCEL_RSP */

    case DIMSE_N_EVENT_REPORT_RQ: return msg.msg.NEventReportRQ.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_N_EVENT_REPORT_RSP: return msg.msg.NEventReportRSP.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_N_GET_RQ: return msg.msg.NGetRQ.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_N_GET_RSP: return msg.msg.NGetRSP.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_N_SET_RQ: return msg.msg.NSetRQ.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_N_SET_RSP: return msg.msg.NSetRSP.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_N_ACTION_RQ: return msg.msg.NActionRQ.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_N_ACTION_RSP: return msg.msg.NActionRSP.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_N_CREATE_RQ: return msg.msg.NCreateRQ.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_N_CREATE_RSP: return msg.msg.NCreateRSP.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_N_DELETE_RQ: return msg.msg.NDeleteRQ.DataSetType != DIMSE_DATASET_NULL;
    case DIMSE_N_DELETE_RSP: return msg.msg.NDeleteRSP.DataSetType != DIMSE_DATASET_NULL;

    default:
        qDebug() << "Unhandled command field" << msg.CommandField;
        break;
    }

    return false;
}

static void dumpIn(T_DIMSE_Message &msg, DcmItem *dataset)
{
    OFString str;
    DIMSE_dumpMessage(str, msg, DIMSE_INCOMING, dataset);
    qDebug() << str.c_str();
}

static void dumpOut(T_DIMSE_Message &msg, DcmItem *dataset)
{
    OFString str;
    DIMSE_dumpMessage(str, msg, DIMSE_OUTGOING, dataset);
    qDebug() << str.c_str();
}

static void copyItems(DcmItem* src, DcmItem *dst)
{
    if (!src)
        return;

    DcmObject* obj = nullptr;
    while (obj = src->nextInContainer(obj), obj != nullptr)
    {
        if (obj->getVR() == EVR_SQ)
        {
            // Ignore ReferencedFilmSessionSequence
            continue;
        }
        dst->insert(dynamic_cast<DcmElement*>(obj->clone()), true);
    }
}

PrintSCP::PrintSCP(QObject *parent)
    : QObject(parent)
    , blockMode(DIMSE_BLOCKING)
    , timeout(0)
    , sessionDataset(nullptr)
    , webServiceCallPerformed(false)
    , assoc(nullptr)
    , upstream(nullptr)
    , ignoreUpstreamErrors(false)
{
    auto oldLocale = setlocale(LC_NUMERIC, "C");
    tess.Init(nullptr, "eng", tesseract::OEM_TESSERACT_ONLY);
    setlocale(LC_NUMERIC, oldLocale);

    QSettings settings;
    blockMode = (T_DIMSE_BlockingMode)settings.value("block-mode", blockMode).toInt();
    timeout   = settings.value("timeout", timeout).toInt();
}

PrintSCP::~PrintSCP()
{
    dropAssociations();
    ASC_dropNetwork(&upstreamNet);
}

DVPSAssociationNegotiationResult PrintSCP::negotiateAssociation(T_ASC_Network *net)
{
    QSettings settings;
    DVPSAssociationNegotiationResult result = DVPSJ_success;
    char buf[BUFSIZ];
    OFBool dropAssoc = OFFalse;

    void *associatePDU=nullptr;
    unsigned long associatePDUlength=0;

    const char *abstractSyntaxes[] =
    {
        UID_BasicGrayscalePrintManagementMetaSOPClass,
        UID_PresentationLUTSOPClass,
        UID_VerificationSOPClass,
    };

    const char* transferSyntaxes[] =
    {
#if __BYTE_ORDER == __LITTLE_ENDIAN
        UID_LittleEndianExplicitTransferSyntax, UID_BigEndianExplicitTransferSyntax,
#elif __BYTE_ORDER == __BIG_ENDIAN
        UID_BigEndianExplicitTransferSyntax, UID_LittleEndianExplicitTransferSyntax,
#else
#error "Unsupported byte order"
#endif
        UID_LittleEndianImplicitTransferSyntax
    };

    OFCondition cond = ASC_receiveAssociation(net, &assoc, DEFAULT_MAXPDU, &associatePDU, &associatePDUlength);
    if (cond.bad())
    {
        qDebug() << "Failed to receive association";
        dropAssoc = OFTrue;
        result = DVPSJ_error;
    }
    else
    {
        printer = assoc->params->DULparams.calledAPTitle;

        qDebug() << "Client association received from "
           << assoc->params->DULparams.callingPresentationAddress
           << ":" << assoc->params->DULparams.callingAPTitle << "=>" << printer;

        ASC_setAPTitles(assoc->params, nullptr, nullptr, printer.toUtf8());

        /* Application Context Name */
        cond = ASC_getApplicationContextName(assoc->params, buf);
        if (cond.bad() || strcmp(buf, DICOM_STDAPPLICATIONCONTEXT) != 0)
        {
            /* reject: the application context name is not supported */
            qDebug() << "Bad AppContextName: " << buf;
            cond = refuseAssociation(ASC_RESULT_REJECTEDTRANSIENT, ASC_REASON_SU_APPCONTEXTNAMENOTSUPPORTED);
            dropAssoc = OFTrue;
            result = DVPSJ_error;
        }
        else if (!settings.childGroups().contains(printer))
        {
          cond = refuseAssociation(ASC_RESULT_REJECTEDTRANSIENT, ASC_REASON_SU_CALLEDAETITLENOTRECOGNIZED);
          dropAssoc = OFTrue;
          result = DVPSJ_error;
        }
        else
        {
            /* accept presentation contexts */
            cond = ASC_acceptContextsWithPreferredTransferSyntaxes(assoc->params,
                abstractSyntaxes, sizeof(abstractSyntaxes)/sizeof(abstractSyntaxes[0]),
                transferSyntaxes, sizeof(transferSyntaxes)/sizeof(transferSyntaxes[0]));
        }
    } /* receiveAssociation successful */

    if (dropAssoc)
    {
        printer.clear();
        dropAssociations();
    }
    else
    {
        sessionDataset = new DcmDataset;

        // Initialize connection to upstream printer

        settings.beginGroup(printer);
        auto printerAetitle  = settings.value("upstream-aetitle").toString();
        auto printerAddress  = settings.value("upstream-address").toString();
        ignoreUpstreamErrors = settings.value("upstream-ignore-errors").toBool();
        settings.endGroup();

        if (printerAetitle.isEmpty())
        {
            qDebug() << "No upstream connection for" << printer;
        }
        else
        {
            DIC_NODENAME localHost;
            T_ASC_Parameters* params = nullptr;

            auto port  = settings.value("print-port", 0).toInt();
            auto cond = ASC_initializeNetwork(NET_REQUESTOR, port, timeout, &upstreamNet);

            qDebug() << "Creating upstream connection to" << printer;

            cond = ASC_createAssociationParameters(&params, settings.value("pdu-size", ASC_DEFAULTMAXPDU).toInt());
            if (cond.good())
            {
                auto appAet = settings.value("aetitle", qApp->applicationName()).toString().toUpper().toUtf8();
                ASC_setAPTitles(params, appAet, printerAetitle.toUtf8(), nullptr);

                // Figure out the presentation addresses and copy the
                // corresponding values into the DcmAssoc parameters.
                //
                gethostname(localHost, sizeof(localHost) - 1);
                ASC_setPresentationAddresses(params, localHost, printerAddress.toUtf8());

                for (size_t i = 0; cond.good() && i < sizeof(abstractSyntaxes)/sizeof(abstractSyntaxes[0]); ++i)
                {
                    cond = ASC_addPresentationContext(params, i*2+1, abstractSyntaxes[i],
                        transferSyntaxes, sizeof(transferSyntaxes)/sizeof(transferSyntaxes[0]));
                }
            }

            if (cond.good())
            {
                cond = ASC_requestAssociation(upstreamNet, params, &upstream);
            }

            if (cond.bad())
            {
                qDebug() << "Failed to create association to" << printerAetitle << cond.text();
                ASC_destroyAssociation(&upstream);
            }
            else
            {
                // Dump general information concerning the establishment of the network connection if required
                //
                qDebug() << "Connection to upstream printer" << printerAetitle
                         << "accepted (max send PDV: " << upstream->sendPDVLength << ")";
            }
        }
    }

    delete[] (char *)associatePDU;
    return result;
}

OFCondition PrintSCP::refuseAssociation(T_ASC_RejectParametersResult result, T_ASC_RejectParametersReason reason)
{
    qDebug() << __FUNCTION__ << result << reason;
    T_ASC_RejectParameters rej = { result, ASC_SOURCE_SERVICEUSER, reason };

    void *associatePDU = nullptr;
    unsigned long associatePDUlength=0;
    OFCondition cond = ASC_rejectAssociation(assoc, &rej, &associatePDU, &associatePDUlength);
    delete[] (char *)associatePDU;
    return cond;
}

void PrintSCP::dropAssociations()
{
    if (assoc)
    {
        qDebug() << "Client connection closed";
        ASC_dropSCPAssociation(assoc);
        ASC_destroyAssociation(&assoc);
    }

    if (upstream)
    {
        qDebug() << "Upstream connection closed";
        ASC_dropSCPAssociation(upstream);
        ASC_destroyAssociation(&upstream);
        ASC_dropNetwork(&upstreamNet);
    }

    delete sessionDataset;
    sessionDataset = nullptr;
    webServiceCallPerformed = false;
}

void PrintSCP::proxyClient()
{
    void *associatePDU = nullptr;
    unsigned long associatePDUlength = 0;

    OFCondition cond = ASC_acknowledgeAssociation(assoc, &associatePDU, &associatePDUlength);
    delete[] (char *)associatePDU;

    /* do real work */
    while (cond.good())
    {
        T_DIMSE_Message rq;
        T_DIMSE_Message rsp;
        T_ASC_PresentationContextID presID;
        T_ASC_PresentationContextID upstreamPresId = 0;
        DcmDataset *rawCommandSet = nullptr;
        DcmDataset *statusDetail = nullptr;
        DcmDataset *rqDataset = nullptr;
        DcmDataset *rspDataset = nullptr;

        cond = DIMSE_receiveCommand(assoc, DIMSE_BLOCKING, 0, &presID, &rq, &statusDetail, &rawCommandSet);

        if (cond.bad())
        {
            qDebug() << "DIMSE_receiveCommand" << cond.text();
            break;
        }

        dump("statusDetail", statusDetail);
        dump("rawCommandSet", rawCommandSet);
        delete rawCommandSet;
        rawCommandSet = nullptr;

        if (isDatasetPresent(rq))
        {
            cond = DIMSE_receiveDataSetInMemory(assoc, blockMode, timeout, &presID, &rqDataset, nullptr, nullptr);
            if (cond.bad())
            {
                qDebug() << "DIMSE_receiveDataSetInMemory" << cond.text();
                break;
            }
        }

        dumpIn(rq, rqDataset);

        cond = DIMSE_sendMessageUsingMemoryData(upstream, presID, &rq, statusDetail, rqDataset, nullptr, nullptr, &rawCommandSet);
        dump("rawCommandSet", rawCommandSet);
        delete rawCommandSet;
        rawCommandSet = nullptr;
        delete statusDetail;
        statusDetail = nullptr;
        delete rqDataset;
        rqDataset = nullptr;

        if (cond.bad())
        {
            qDebug() << "DIMSE_sendMessageUsingMemoryData(upstream) failed" << cond.text();
            break;
        }

        cond = DIMSE_receiveCommand(upstream, blockMode, timeout, &upstreamPresId, &rsp, &statusDetail, &rawCommandSet);
        dump("rawCommandSet", rawCommandSet);
        delete rawCommandSet;
        rawCommandSet = nullptr;
        dump("statusDetail", statusDetail);

        if (cond.bad())
        {
            qDebug() << "DIMSE_recv(upstream) failed" << cond.text();
            break;
        }

        if (rq.CommandField != (rsp.CommandField & ~0x8000))
        {
            qDebug() << "Mismatched response: rq" << rq.CommandField << "rsp" << rsp.CommandField;
        }

        if (isDatasetPresent(rsp))
        {
            cond = DIMSE_receiveDataSetInMemory(upstream, blockMode, timeout, &upstreamPresId, &rspDataset, nullptr, nullptr);
            if (cond.bad())
            {
                qDebug() << "DIMSE_receiveDataSetInMemory(upstream)" << cond.text();
                break;
            }
        }

        dumpOut(rsp, rspDataset);

        cond = DIMSE_sendMessageUsingMemoryData(assoc, presID, &rsp, statusDetail, rspDataset, nullptr, nullptr, &rawCommandSet);
        dump("rawCommandSet", rawCommandSet);
        delete rawCommandSet;
        rawCommandSet = nullptr;
        delete statusDetail;
        statusDetail = nullptr;
        delete rspDataset;
        rspDataset = nullptr;

        if (cond.bad())
        {
            qDebug() << "DIMSE_sendMessageUsingMemoryData" << cond.text();
            break;
        }
    } /* while */

    qDebug() << "Done";

    // close client association
    //
    if (cond == DUL_PEERREQUESTEDRELEASE)
    {
        qDebug() << "Association Release";
        cond = ASC_acknowledgeRelease(assoc);
    }
    else if (cond == DUL_PEERABORTEDASSOCIATION)
    {
        qDebug() << "Association Aborted";
    }
    else
    {
      qDebug() << "DIMSE Failure (aborting association)";
      cond = ASC_abortAssociation(assoc);
    }

    // close upstream printer association
    //
    if (upstream)
    {
        ASC_releaseAssociation(upstream);
    }

    dropAssociations();
}

void PrintSCP::handleClient()
{
    void *associatePDU = nullptr;
    unsigned long associatePDUlength = 0;

    OFCondition cond = ASC_acknowledgeAssociation(assoc, &associatePDU, &associatePDUlength);
    delete[] (char *)associatePDU;

    /* do real work */
    while (cond.good())
    {
        T_DIMSE_Message msg;
        T_ASC_PresentationContextID presID;
        DcmDataset *rawCommandSet=nullptr;
        cond = DIMSE_receiveCommand(assoc, DIMSE_BLOCKING, 0, &presID, &msg, nullptr, &rawCommandSet);
        delete rawCommandSet;

        if (cond.good())
        {
            /* process command */
            switch (msg.CommandField)
            {
            case DIMSE_C_ECHO_RQ:
                cond = handleCEcho(msg, presID);
                break;
            case DIMSE_N_GET_RQ:
                cond = handleNGet(msg, presID);
                break;
            case DIMSE_N_SET_RQ:
                cond = handleNSet(msg, presID);
                break;
            case DIMSE_N_ACTION_RQ:
                cond = handleNAction(msg, presID);
                break;
            case DIMSE_N_CREATE_RQ:
                cond = handleNCreate(msg, presID);
                break;
            case DIMSE_N_DELETE_RQ:
                cond = handleNDelete(msg, presID);
                break;
            default:
                cond = DIMSE_BADCOMMANDTYPE; /* unsupported command */
                qDebug() << "Cannot handle command: 0x" << QString::number((unsigned)msg.CommandField, 16);
                break;
            }
        }
    } /* while */

    // close client association
    //
    if (cond == DUL_PEERREQUESTEDRELEASE)
    {
        qDebug() << "Association Release";
        cond = ASC_acknowledgeRelease(assoc);
    }
    else if (cond == DUL_PEERABORTEDASSOCIATION)
    {
        qDebug() << "Association Aborted";
    }
    else
    {
      qDebug() << "DIMSE Failure (aborting association)";
      cond = ASC_abortAssociation(assoc);
    }

    // close upstream printer association
    //
    if (upstream)
    {
        ASC_releaseAssociation(upstream);
    }

    dropAssociations();
}

OFCondition PrintSCP::handleCEcho(T_DIMSE_Message& rq, T_ASC_PresentationContextID presID)
{
    OFCondition cond = EC_Normal;
    DIC_US status = STATUS_Success;

    if (upstream)
    {
        DIC_US msgId = upstream->nextMsgID++;
        cond = DIMSE_echoUser(upstream, msgId, blockMode, timeout, &status, nullptr);
        if (cond.bad()) qDebug() << "DIMSE_echoUser(upstream) failed" << cond.text();
    }

    if (cond.good() || ignoreUpstreamErrors)
    {
        cond = DIMSE_sendEchoResponse(assoc, presID, &rq.msg.CEchoRQ, status, nullptr);
    }
    return cond;
}

OFCondition PrintSCP::handleNGet(T_DIMSE_Message& rq, T_ASC_PresentationContextID presID)
{
    // initialize response message
    T_DIMSE_Message rsp;
    rsp.CommandField = DIMSE_N_GET_RSP;
    rsp.msg.NGetRSP.MessageIDBeingRespondedTo = rq.msg.NGetRQ.MessageID;
    rsp.msg.NGetRSP.AffectedSOPClassUID[0] = 0;
    rsp.msg.NGetRSP.DimseStatus = STATUS_Success;
    rsp.msg.NGetRSP.AffectedSOPInstanceUID[0] = 0;
    rsp.msg.NGetRSP.DataSetType = DIMSE_DATASET_NULL;
    rsp.msg.NGetRSP.opts = 0;

    OFCondition cond = EC_Normal;
    DcmDataset *rqDataset = nullptr;
    DcmDataset *rspDataset = nullptr;
    DcmDataset *rspCommand = nullptr;

    if (rq.msg.NGetRQ.DataSetType == DIMSE_DATASET_PRESENT)
    {
        // should not happen
        cond = DIMSE_receiveDataSetInMemory(assoc, blockMode, timeout, &presID, &rqDataset, nullptr, nullptr);
    }
    dumpIn(rq, rqDataset);

    if (cond.good() && upstream)
    {
        T_ASC_PresentationContextID upstreamPresId = 0;
        cond = DIMSE_sendMessageUsingMemoryData(upstream, presID, &rq, nullptr, rqDataset, nullptr, nullptr, &rspCommand);
        if (cond.bad()) qDebug() << "DIMSE_send(upstream, N-Get) failed" << cond.text();
        delete rspCommand;
        delete rqDataset;
        rqDataset = nullptr;

        cond = DIMSE_receiveCommand(upstream, blockMode, timeout, &upstreamPresId, &rsp, &rspDataset, &rspCommand);
        if (cond.bad()) qDebug() << "DIMSE_recv(upstream, N-Get) failed" << cond.text();
        delete rspCommand;
        delete rspDataset;
        rspDataset = nullptr;

        if (cond.good() && rsp.CommandField == DIMSE_N_GET_RSP)
        {
            if (rsp.msg.NGetRSP.DataSetType == DIMSE_DATASET_PRESENT)
            {
                cond = DIMSE_receiveDataSetInMemory(upstream, blockMode, timeout, &upstreamPresId, &rspDataset, nullptr, nullptr);
            }
            if (cond.good())
            {
                copyItems(rspDataset, sessionDataset);
                cond = DIMSE_sendMessageUsingMemoryData(assoc, presID, &rsp, nullptr, rspDataset, nullptr, nullptr, &rspCommand);
                dumpOut(rsp, rspDataset);
                delete rspCommand;
            }
            delete rspDataset;
            rspDataset = nullptr;
        }

        if (ignoreUpstreamErrors)
            cond = EC_Normal;
    }

    delete rqDataset;

    if (cond.bad())
        return cond;

    QString sopClass(rq.msg.NGetRQ.RequestedSOPClassUID);
    if (sopClass == UID_PrinterSOPClass)
    {
        // Print N-GET
        printerNGet(rq, rsp, rspDataset);
    }
    else
    {
        qDebug() << "N-GET unsupported for SOP class '" << sopClass << "'";
        rsp.msg.NGetRSP.DimseStatus = STATUS_N_NoSuchSOPClass;
    }

    if (!upstream)
    {
        copyItems(rspDataset, sessionDataset);
        cond = DIMSE_sendMessageUsingMemoryData(assoc, presID, &rsp, nullptr, rspDataset, nullptr, nullptr, &rspCommand);
        delete rspCommand;
        dumpOut(rsp, rspDataset);
    }

    delete rspDataset;
    return cond;
}

OFCondition PrintSCP::handleNSet(T_DIMSE_Message& rq, T_ASC_PresentationContextID presID)
{
    // initialize response message
    T_DIMSE_Message rsp;
    rsp.CommandField = DIMSE_N_SET_RSP;
    rsp.msg.NSetRSP.MessageIDBeingRespondedTo = rq.msg.NSetRQ.MessageID;
    rsp.msg.NSetRSP.AffectedSOPClassUID[0] = 0;
    rsp.msg.NSetRSP.DimseStatus = STATUS_Success;
    rsp.msg.NSetRSP.AffectedSOPInstanceUID[0] = 0;
    rsp.msg.NSetRSP.DataSetType = DIMSE_DATASET_NULL;
    rsp.msg.NSetRSP.opts = 0;

    OFCondition cond = EC_Normal;
    DcmDataset *dataset = nullptr;
    DcmDataset *rqDataset = nullptr;
    DcmDataset *rspDataset = nullptr;
    DcmDataset *rspCommand = nullptr;

    if (rq.msg.NSetRQ.DataSetType == DIMSE_DATASET_PRESENT)
    {
        cond = DIMSE_receiveDataSetInMemory(assoc, blockMode, timeout, &presID, &rqDataset, nullptr, nullptr);
    }
    dumpIn(rq, rqDataset);

    if (cond.good() && upstream)
    {
        T_ASC_PresentationContextID upstreamPresId = 0;
        cond = DIMSE_sendMessageUsingMemoryData(upstream, presID, &rq, nullptr, rqDataset, nullptr, nullptr, &rspCommand);
        if (cond.bad()) qDebug() << "DIMSE_send(upstream, N-Set) failed" << cond.text();
        delete rspCommand;

        cond = DIMSE_receiveCommand(upstream, blockMode, timeout, &upstreamPresId, &rsp, &dataset, &rspCommand);
        if (cond.bad()) qDebug() << "DIMSE_recv(upstream, N-Set) failed" << cond.text();
        delete dataset;
        dataset = nullptr;
        delete rspCommand;

        if (cond.good() && rsp.CommandField == DIMSE_N_SET_RSP)
        {
            if (rsp.msg.NSetRSP.DataSetType == DIMSE_DATASET_PRESENT)
            {
                cond = DIMSE_receiveDataSetInMemory(upstream, blockMode, timeout, &upstreamPresId, &dataset, nullptr, nullptr);
            }
            if (cond.good())
            {
                cond = DIMSE_sendMessageUsingMemoryData(assoc, presID, &rsp, nullptr, dataset, nullptr, nullptr, &rspCommand);
                dumpOut(rsp, dataset);
                delete rspCommand;
            }
            delete dataset;
        }

        if (ignoreUpstreamErrors)
            cond = EC_Normal;
    }

    if (cond.bad())
    {
        delete rqDataset;
        return cond;
    }

    QString sopClass(rq.msg.NSetRQ.RequestedSOPClassUID);
    if (sopClass == UID_BasicFilmSessionSOPClass)
    {
        // BFS N-SET
        filmSessionNSet(rq, rqDataset, rsp, rspDataset);
    }
    else if (sopClass == UID_BasicFilmBoxSOPClass)
    {
        // BFB N-SET
        filmBoxNSet(rq, rqDataset, rsp, rspDataset);
    }
    else if (sopClass == UID_BasicGrayscaleImageBoxSOPClass)
    {
        // BGIB N-SET
        imageBoxNSet(rq, rqDataset, rsp, rspDataset);
    }
    else
    {
        qDebug() << "N-SET unsupported for SOP class '" << sopClass << "'";
        rsp.msg.NSetRSP.DimseStatus = STATUS_N_NoSuchSOPClass;
    }

    if (!upstream)
    {
        cond = DIMSE_sendMessageUsingMemoryData(assoc, presID, &rsp, nullptr, rspDataset, nullptr, nullptr, &rspCommand);
        dumpOut(rsp, rspDataset);
        delete rspCommand;
    }

    delete rqDataset;
    delete rspDataset;
    return cond;
}


OFCondition PrintSCP::handleNAction(T_DIMSE_Message& rq, T_ASC_PresentationContextID presID)
{
    // initialize response message
    T_DIMSE_Message rsp;
    rsp.CommandField = DIMSE_N_ACTION_RSP;
    rsp.msg.NActionRSP.MessageIDBeingRespondedTo = rq.msg.NActionRQ.MessageID;
    rsp.msg.NActionRSP.AffectedSOPClassUID[0] = 0;
    rsp.msg.NActionRSP.DimseStatus = STATUS_Success;
    rsp.msg.NActionRSP.AffectedSOPInstanceUID[0] = 0;
    rsp.msg.NActionRSP.ActionTypeID = rq.msg.NActionRQ.ActionTypeID;
    rsp.msg.NActionRSP.DataSetType = DIMSE_DATASET_NULL;
    rsp.msg.NActionRSP.opts = O_NACTION_ACTIONTYPEID;

    OFCondition cond = EC_Normal;
    DcmDataset *rqDataset = nullptr;
    DcmDataset *rspCommand = nullptr;
    DcmDataset *rspDataset = nullptr;

    if (rq.msg.NActionRQ.DataSetType == DIMSE_DATASET_PRESENT)
    {
        cond = DIMSE_receiveDataSetInMemory(assoc, blockMode, timeout, &presID, &rqDataset, nullptr, nullptr);
    }
    dumpIn(rq, rqDataset);

    if (cond.good() && upstream)
    {
        T_ASC_PresentationContextID upstreamPresId = 0;
        cond = DIMSE_sendMessageUsingMemoryData(upstream, presID, &rq, nullptr, rqDataset, nullptr, nullptr, &rspCommand);
        if (cond.bad()) qDebug() << "DIMSE_send(upstream, N-Action) failed" << cond.text();
        delete rspCommand;

        cond = DIMSE_receiveCommand(upstream, blockMode, timeout, &upstreamPresId, &rsp, &rspDataset, &rspCommand);
        if (cond.bad()) qDebug() << "DIMSE_recv(upstream, N-Action) failed" << cond.text();
        delete rspDataset;
        rspDataset = nullptr;
        delete rspCommand;

        if (cond.good() && rsp.CommandField == DIMSE_N_ACTION_RSP)
        {
          if (rsp.msg.NActionRSP.DataSetType == DIMSE_DATASET_PRESENT)
          {
              cond = DIMSE_receiveDataSetInMemory(upstream, blockMode, timeout, &upstreamPresId, &rspDataset, nullptr, nullptr);
          }
          if (cond.good())
          {
              cond = DIMSE_sendMessageUsingMemoryData(assoc, presID, &rsp, nullptr, rspDataset, nullptr, nullptr, &rspCommand);
              dumpOut(rsp, rspDataset);
              delete rspCommand;
          }
          delete rspDataset;
          rspDataset = nullptr;
        }

        if (ignoreUpstreamErrors)
            cond = EC_Normal;
    }
    delete rqDataset;
    rqDataset = nullptr;

    if (cond.bad())
    {
        return cond;
    }

    QString sopClass(rq.msg.NActionRQ.RequestedSOPClassUID);
    if (sopClass == UID_BasicFilmSessionSOPClass)
    {
        // BFS N-ACTION
        filmSessionNAction(rq, rsp);
    }
    else if (sopClass == UID_BasicFilmBoxSOPClass)
    {
        // BFB N-ACTION
        filmBoxNAction(rq, rsp);
    }
    else
    {
        qDebug() << "N-ACTION unsupported for SOP class '" << sopClass << "'";
        rsp.msg.NActionRSP.DimseStatus = STATUS_N_NoSuchSOPClass;
    }

    if (!upstream)
    {
        cond = DIMSE_sendMessageUsingMemoryData(assoc, presID, &rsp, nullptr, nullptr, nullptr, nullptr, &rspCommand);
        delete rspCommand;
    }
    return cond;
}

OFCondition PrintSCP::handleNCreate(T_DIMSE_Message& rq, T_ASC_PresentationContextID presID)
{
    // initialize response message
    T_DIMSE_Message rsp;
    rsp.CommandField = DIMSE_N_CREATE_RSP;
    rsp.msg.NCreateRSP.MessageIDBeingRespondedTo = rq.msg.NCreateRQ.MessageID;
    rsp.msg.NCreateRSP.AffectedSOPClassUID[0] = 0;
    rsp.msg.NCreateRSP.DimseStatus = STATUS_Success;
    if (rq.msg.NCreateRQ.opts & O_NCREATE_AFFECTEDSOPINSTANCEUID)
    {
        // instance UID is provided by SCU
        strncpy(rsp.msg.NCreateRSP.AffectedSOPInstanceUID, rq.msg.NCreateRQ.AffectedSOPInstanceUID, sizeof(DIC_UI));
    }
    else
    {
        // we generate our own instance UID
        dcmGenerateUniqueIdentifier(rsp.msg.NCreateRSP.AffectedSOPInstanceUID);
    }
    rsp.msg.NCreateRSP.DataSetType = DIMSE_DATASET_NULL;
    rsp.msg.NCreateRSP.opts = O_NCREATE_AFFECTEDSOPINSTANCEUID | O_NCREATE_AFFECTEDSOPCLASSUID;
    strncpy(rsp.msg.NCreateRSP.AffectedSOPClassUID, rq.msg.NCreateRQ.AffectedSOPClassUID, sizeof(DIC_UI));

    OFCondition cond = EC_Normal;
    DcmDataset *rqDataset = nullptr;
    DcmDataset *rspDataset = nullptr;
    DcmDataset *rspCommand = nullptr;

    if (rq.msg.NCreateRQ.DataSetType == DIMSE_DATASET_PRESENT)
    {
        cond = DIMSE_receiveDataSetInMemory(assoc, blockMode, timeout, &presID, &rqDataset, nullptr, nullptr);
        copyItems(rqDataset, sessionDataset);
    }
    dumpIn(rq, rqDataset);

    if (cond.good() && upstream)
    {
        T_ASC_PresentationContextID upstreamPresId = 0;
        cond = DIMSE_sendMessageUsingMemoryData(upstream, presID, &rq, nullptr, rqDataset, nullptr, nullptr, &rspCommand);
        if (cond.bad()) qDebug() << "DIMSE_send(upstream, N-Create) failed" << cond.text();
        delete rspCommand;

        cond = DIMSE_receiveCommand(upstream, blockMode, timeout, &upstreamPresId, &rsp, &rspDataset, &rspCommand);
        if (cond.bad()) qDebug() << "DIMSE_recv(upstream, N-Create) failed" << cond.text();
        delete rspDataset;
        rspDataset = nullptr;
        delete rspCommand;

        if (cond.good() && rsp.CommandField == DIMSE_N_CREATE_RSP)
        {
            if (rsp.msg.NCreateRSP.DataSetType == DIMSE_DATASET_PRESENT)
            {
                cond = DIMSE_receiveDataSetInMemory(upstream, blockMode, timeout, &upstreamPresId, &rspDataset, nullptr, nullptr);
            }
            if (cond.good())
            {
                cond = DIMSE_sendMessageUsingMemoryData(assoc, presID, &rsp, nullptr, rspDataset, nullptr, nullptr, &rspCommand);
                dumpOut(rsp, rspDataset);
                delete rspCommand;
            }
            delete rspDataset;
            rspDataset = nullptr;
        }

        if (ignoreUpstreamErrors)
            cond = EC_Normal;
    }

    if (cond.bad())
    {
        delete rqDataset;
        return cond;
    }

    QString sopClass(rq.msg.NCreateRQ.AffectedSOPClassUID);
    if (sopClass == UID_BasicFilmSessionSOPClass)
    {
        // BFS N-CREATE
        filmSessionNCreate(rqDataset, rsp, rspDataset);
    }
    else if (sopClass == UID_BasicFilmBoxSOPClass)
    {
        // BFB N-CREATE
        filmBoxNCreate(rqDataset, rsp, rspDataset);
    }
    else if (sopClass == UID_PresentationLUTSOPClass)
    {
        // P-LUT N-CREATE
        presentationLUTNCreate(rqDataset, rsp, rspDataset);
    }
    else
    {
        qDebug() << "N-CREATE unsupported for SOP class '" << sopClass << "'";
        rsp.msg.NCreateRSP.DimseStatus = STATUS_N_NoSuchSOPClass;
        rsp.msg.NCreateRSP.opts = 0;  // don't include affected SOP instance UID
    }

    if (!upstream)
    {
        cond = DIMSE_sendMessageUsingMemoryData(assoc, presID, &rsp, nullptr, rspDataset, nullptr, nullptr, &rspCommand);
        dumpOut(rsp, rspDataset);
        delete rspCommand;
    }
    delete rqDataset;
    delete rspDataset;
    return cond;
}

OFCondition PrintSCP::handleNDelete(T_DIMSE_Message& rq, T_ASC_PresentationContextID presID)
{
    // initialize response message
    T_DIMSE_Message rsp;
    rsp.CommandField = DIMSE_N_DELETE_RSP;
    rsp.msg.NDeleteRSP.MessageIDBeingRespondedTo = rq.msg.NDeleteRQ.MessageID;
    rsp.msg.NDeleteRSP.AffectedSOPClassUID[0] = 0;
    rsp.msg.NDeleteRSP.DimseStatus = STATUS_Success;
    rsp.msg.NDeleteRSP.AffectedSOPInstanceUID[0] = 0;
    rsp.msg.NDeleteRSP.DataSetType = DIMSE_DATASET_NULL;
    rsp.msg.NDeleteRSP.opts = 0;

    OFCondition cond = EC_Normal;
    DcmDataset *dataset = nullptr;
    DcmDataset *rspCommand = nullptr;
    if (rq.msg.NDeleteRQ.DataSetType == DIMSE_DATASET_PRESENT)
    {
        // should not happen
        cond = DIMSE_receiveDataSetInMemory(assoc, blockMode, timeout, &presID, &dataset, nullptr, nullptr);
    }
    dumpIn(rq, dataset);

    if (cond.good() && upstream)
    {
      T_ASC_PresentationContextID upstreamPresId = 0;
      cond = DIMSE_sendMessageUsingMemoryData(upstream, presID, &rq, nullptr, dataset, nullptr, nullptr, &rspCommand);
      if (cond.bad()) qDebug() << "DIMSE_send(upstream, N-Delete) failed" << cond.text();
      delete rspCommand;

      cond = DIMSE_receiveCommand(upstream, blockMode, timeout, &upstreamPresId, &rsp, &dataset, &rspCommand);
      if (cond.bad()) qDebug() << "DIMSE_recv(upstream, N-Delete) failed" << cond.text();
      delete dataset;
      dataset = nullptr;
      delete rspCommand;

      if (cond.good() && rsp.CommandField == DIMSE_N_DELETE_RSP)
      {
          if (rsp.msg.NDeleteRSP.DataSetType == DIMSE_DATASET_PRESENT)
          {
              cond = DIMSE_receiveDataSetInMemory(upstream, blockMode, timeout, &upstreamPresId, &dataset, nullptr, nullptr);
          }
          if (cond.good())
          {
              cond = DIMSE_sendMessageUsingMemoryData(assoc, presID, &rsp, nullptr, dataset, nullptr, nullptr, &rspCommand);
              dumpOut(rsp, dataset);
              delete rspCommand;
          }
      }
      if (ignoreUpstreamErrors)
          cond = EC_Normal;
    }
    delete dataset;

    if (cond.bad())
        return cond;

    QString sopClass(rq.msg.NDeleteRQ.RequestedSOPClassUID);
    if (sopClass == UID_BasicFilmSessionSOPClass)
    {
        // BFS N-DELETE
        filmSessionNDelete(rq, rsp);
    }
    else if (sopClass == UID_BasicFilmBoxSOPClass)
    {
        // BFB N-DELETE
        filmBoxNDelete(rq, rsp);
    }
    else
    {
        qDebug() << "N-DELETE unsupported for SOP class '" << sopClass << "'";
        rsp.msg.NDeleteRSP.DimseStatus = STATUS_N_NoSuchSOPClass;
    }

    if (!upstream)
    {
        cond = DIMSE_sendMessageUsingMemoryData(assoc, presID, &rsp, nullptr, nullptr, nullptr, nullptr, &rspCommand);
        dumpOut(rsp, dataset);
        delete rspCommand;
    }
    return cond;
}

void PrintSCP::printerNGet(T_DIMSE_Message& rq, T_DIMSE_Message& rsp, DcmDataset *& rspDataset)
{
    QString printerInstance(UID_PrinterSOPInstance);
    if (printerInstance == rq.msg.NGetRQ.RequestedSOPInstanceUID)
    {
        rsp.msg.NSetRSP.DataSetType = DIMSE_DATASET_PRESENT;
        rspDataset = new DcmDataset;

        // By default, send only PrinterStatus & PrinterStatusInfo
        //
        if (rq.msg.NGetRQ.ListCount == 0)
        {
            rspDataset->putAndInsertString(DCM_PrinterStatus, DEFAULT_printerStatus);
            rspDataset->putAndInsertString(DCM_PrinterStatusInfo, DEFAULT_printerStatusInfo);
        }
        else
        {
            QSettings settings;
            settings.beginGroup(printer);
            QMap<DcmTag, QVariant> info;
            auto size = settings.beginReadArray("info");
            for (int idx = 0; idx < size; ++idx)
            {
                settings.setArrayIndex(idx);
                auto key = settings.value("key").toString();
                DcmTag tag;
                if (DcmTag::findTagFromName(key.toUtf8(), tag).good())
                {
                    info[tag] = settings.value("value");
                }
                else
                {
                    qDebug() << "Bad DICOM tag" << key << "in" << printer << "info" << idx;
                }
            }
            settings.endArray();
            settings.endGroup();

            for (int i = 0; i < rq.msg.NGetRQ.ListCount / 2; ++i)
            {
                auto group   = rq.msg.NGetRQ.AttributeIdentifierList[i*2];
                auto element = rq.msg.NGetRQ.AttributeIdentifierList[i*2 + 1];
                if (element == 0x0000)
                {
                    // Group length
                    //
                    continue;
                }

                if (group == DCM_PrinterStatus.getGroup())
                {
                    if (element == DCM_PrinterStatus.getElement())
                    {
                        rspDataset->putAndInsertString(DCM_PrinterStatus, DEFAULT_printerStatus);
                        continue;
                    }
                    if (element == DCM_PrinterStatusInfo.getElement())
                    {
                        rspDataset->putAndInsertString(DCM_PrinterStatusInfo, DEFAULT_printerStatusInfo);
                        continue;
                    }
                }

                // Some unknown element was requested.
                //
                DcmTag tag(group, element);
                if (!info.contains(tag))
                {
                    qDebug() << "cannot retrieve printer information: unknown attribute ("
                        << QString::number(group, 16) << "," << QString::number(element, 16)
                        << ") in attribute list.";
                    rsp.msg.NGetRSP.DimseStatus = STATUS_N_NoSuchAttribute;
                    delete rspDataset;
                    rspDataset = nullptr;
                    break;
                }

                if (tag.getVR().isaString())
                {
                    rspDataset->putAndInsertString(tag, info[tag].toString().toUtf8());
                }
                else
                {
                    //TODO: implement other types then strings
                    qDebug() << "VR" << tag.getVRName() << "not implemented";
                }
            }
        }
    }
    else
    {
        qDebug() << "cannot retrieve printer information, unknown printer SOP instance UID" << printerInstance;
        rsp.msg.NGetRSP.DimseStatus = STATUS_N_NoSuchObjectInstance;
    }
}

void PrintSCP::filmSessionNSet(T_DIMSE_Message&, DcmDataset *, T_DIMSE_Message&, DcmDataset *&)
{
}

void PrintSCP::filmBoxNSet(T_DIMSE_Message&, DcmDataset *, T_DIMSE_Message&, DcmDataset *&)
{
}

void PrintSCP::filmSessionNAction(T_DIMSE_Message&, T_DIMSE_Message&)
{
}

void PrintSCP::filmBoxNAction(T_DIMSE_Message&, T_DIMSE_Message&)
{
}

void PrintSCP::filmSessionNCreate(DcmDataset *, T_DIMSE_Message& rsp, DcmDataset *&)
{
    if (filmSessionUID.isEmpty())
    {
        filmSessionUID = rsp.msg.NCreateRSP.AffectedSOPInstanceUID;

        char uid[100];
        dcmGenerateUniqueIdentifier(uid,  SITE_STUDY_UID_ROOT);
        studyInstanceUID = uid;
        dcmGenerateUniqueIdentifier(uid,  SITE_SERIES_UID_ROOT);
        seriesInstanceUID = uid;
    }
    else
    {
        // film session exists already, refuse n-create
        qDebug() << "cannot create two film sessions concurrently.";
        rsp.msg.NCreateRSP.DimseStatus = STATUS_N_DuplicateSOPInstance;
        rsp.msg.NCreateRSP.opts = 0;  // don't include affected SOP instance UID
    }
}

void PrintSCP::filmBoxNCreate(DcmDataset *rqDataset, T_DIMSE_Message& rsp, DcmDataset *& rspDataset)
{
    if (!filmSessionUID.isEmpty())
    {
        rsp.msg.NCreateRSP.DataSetType = DIMSE_DATASET_PRESENT;
        rspDataset = (DcmDataset*)rqDataset->clone();
        auto dseq = new DcmSequenceOfItems(DCM_ReferencedImageBoxSequence);
        auto ditem = new DcmItem();
        ditem->putAndInsertString(DCM_ReferencedSOPClassUID, UID_BasicGrayscaleImageBoxSOPClass);
        char uid[100];
        ditem->putAndInsertString(DCM_ReferencedSOPInstanceUID, dcmGenerateUniqueIdentifier(uid, SITE_INSTANCE_UID_ROOT));
        dseq->insert(ditem);
        rspDataset->insert(dseq);
    }
    else
    {
        // no film session, refuse n-create
        qDebug() << "cannot create film box without film session.";
        rsp.msg.NCreateRSP.DimseStatus = STATUS_N_InvalidObjectInstance;
        rsp.msg.NCreateRSP.opts = 0;  // don't include affected SOP instance UID
    }
}

void PrintSCP::presentationLUTNCreate(DcmDataset *, T_DIMSE_Message&, DcmDataset *&)
{
}

void PrintSCP::filmSessionNDelete(T_DIMSE_Message& rq, T_DIMSE_Message& rsp)
{
  if (filmSessionUID == rq.msg.NDeleteRQ.RequestedSOPInstanceUID)
  {
    filmSessionUID.clear();
    studyInstanceUID.clear();
    seriesInstanceUID.clear();
  }
  else
  {
    // film session does not exist or wrong instance UID
    qDebug() << "cannot delete film session with instance UID '" << rq.msg.NDeleteRQ.RequestedSOPInstanceUID << "': object does not exist.";
    rsp.msg.NDeleteRSP.DimseStatus = STATUS_N_NoSuchObjectInstance;
  }
}

void PrintSCP::filmBoxNDelete(T_DIMSE_Message&, T_DIMSE_Message&)
{
}

void PrintSCP::imageBoxNSet(T_DIMSE_Message&, DcmDataset *rqDataset, T_DIMSE_Message&, DcmDataset *&)
{
    if (!rqDataset)
    {
        qDebug() << __FUNCTION__ << "Request dataset is missing";
        return;
    }

    DcmItem *item = nullptr;
    auto cond = rqDataset->findAndGetSequenceItem(DCM_BasicGrayscaleImageSequence, item);
    if (cond.good())
    {
        // Pull up children items from sequence to the dataset
        //
        DcmElement* obj = nullptr;
        while (obj = item->remove(0UL), obj != nullptr)
        {
            rqDataset->insert(obj);
        }
        rqDataset->remove(DCM_BasicGrayscaleImageSequence);
        delete item;
    }

    rqDataset->putAndInsertString(DCM_SpecificCharacterSet, "ISO_IR 192"); // UTF-8

    // Add all required UIDs to the image
    //
    char instanceUID[100] = {0};
    dcmGenerateUniqueIdentifier(instanceUID,  SITE_INSTANCE_UID_ROOT);
    rqDataset->putAndInsertString(DCM_SOPInstanceUID,    instanceUID);
    rqDataset->putAndInsertString(DCM_StudyInstanceUID,  studyInstanceUID.toUtf8());
    rqDataset->putAndInsertString(DCM_SeriesInstanceUID, seriesInstanceUID.toUtf8());

    auto now = QDateTime::currentDateTime();
    rqDataset->putAndInsertString(DCM_InstanceCreationDate, now.toString("yyyyMMdd").toUtf8());
    rqDataset->putAndInsertString(DCM_InstanceCreationTime, now.toString("HHmmss").toUtf8());
    rqDataset->putAndInsertString(DCM_StudyDate, now.toString("yyyyMMdd").toUtf8());
    rqDataset->putAndInsertString(DCM_StudyTime, now.toString("HHmmss").toUtf8());

    rqDataset->putAndInsertString(DCM_Manufacturer, ORGANIZATION_FULL_NAME);
    rqDataset->putAndInsertString(DCM_ManufacturerModelName, PRODUCT_FULL_NAME);

    QSettings settings;
    QMap<QString, QString> queryParams;
    DicomImage di(rqDataset, rqDataset->getOriginalXfer());
    void *data;
    if (di.createJavaAWTBitmap(data, 0, 32) && data)
    {
        tess.SetImage((const unsigned char*)data, di.getWidth(), di.getHeight(), 4, 4 * di.getWidth());

        // Global tags
        //
        insertTags(queryParams, &di, settings);

        // This printer tags
        //
        settings.beginGroup(printer);
        insertTags(queryParams, &di, settings);
        settings.endGroup();
        delete[] (Uint32*)data;
    }

    if (!webServiceCallPerformed)
    {
        webServiceCallPerformed = true;
        webQuery(queryParams);
    }

    copyItems(sessionDataset, rqDataset);

    foreach (auto server, settings.value("storage-servers").toStringList())
    {
        StoreSCP sscp(server);
        cond = sscp.sendToServer(rqDataset, instanceUID);
        if (cond.bad())
        {
            qDebug() << "Failed to store to" << server << cond.text();
        }
    }

    if (settings.value("debug").toBool())
    {
        DcmFileFormat ff(rqDataset);
        cond = ff.saveFile("rq.dcm", EXS_LittleEndianExplicit,  EET_ExplicitLength, EGL_recalcGL, EPD_withoutPadding);
        if (cond.bad())
        {
            qDebug() << "Failed to save rq.dcm" << cond.text();
        }
    }
}

void PrintSCP::webQuery(const QMap<QString, QString>& queryParams)
{
    QSettings settings;

    auto url = settings.value("query-url").toUrl();
    auto userName = settings.value("username").toString();
    auto password = settings.value("password").toString();

    settings.beginGroup(printer);
    url = settings.value("query-url", url).toUrl();
    userName = settings.value("username", userName).toString();
    password = settings.value("password", password).toString();
    settings.endGroup();

    if (url.isEmpty())
    {
        return;
    }

#if (QT_VERSION >= QT_VERSION_CHECK(5, 0, 0))
    QUrlQuery query(url);
#else
#define query url
#endif
    query.addQueryItem("studyInstanceUID", filmSessionUID);
    query.addQueryItem("examinationDate", QDate::currentDate().toString("yyyy-MM-dd"));

    for (auto i = queryParams.constBegin(); i != queryParams.constEnd(); ++i)
    {
        query.addQueryItem(i.key(), i.value());
    }

#if (QT_VERSION >= QT_VERSION_CHECK(5, 0, 0))
    url.setQuery(query);
#else
#undef query
#endif

    QNetworkRequest rq(url);
    if (!userName.isEmpty())
    {
        rq.setRawHeader("Authorization", "Basic " + QByteArray(userName.append(':').append(password).toUtf8()).toBase64());
    }

    bool error = false;
    QNetworkAccessManager mgr;
    auto reply = mgr.get(rq);
    while (reply->isRunning())
    {
        qApp->processEvents(QEventLoop::AllEvents, 1000);
    }

    if (reply->error())
    {
        qDebug() << "Error loading " << url << reply->errorString() << reply->error();
        ++error;
    }
    else
    {
        auto response = reply->readAll();
        if (settings.value("debug").toBool())
        {
            qDebug() << QString::fromUtf8(response);
        }

        QXmlStreamReader xml(response);
        while (xml.readNextStartElement())
        {
            if (xml.name() == "element")
            {
                DcmTag tag;
                auto key = xml.attributes().value("tag");
                if (DcmTag::findTagFromName(key.toUtf8(), tag).bad())
                {
                    qDebug() << "Unknown DCM tag" << key;
                }
                else
                {
                    auto str = translateToLatin(xml.readElementText());
                    qDebug() << tag.getXTag().toString().c_str() << tag.getTagName() << str;
                    sessionDataset->putAndInsertString(tag, str.toUtf8());
                }
            }
            else if (xml.name() == "data-set")
            {
                continue;
            }
            else
            {
                qDebug() << "Unexpected element" << xml.name();
                ++error;
            }
        }

        if (xml.hasError())
        {
            qDebug() << "XML parse error" << xml.errorString();
            ++error;
        }
    }

    if (error)
    {
        // In case of eny error set PatientId to '0'
        //
        sessionDataset->putAndInsertString(DCM_PatientID, "0");
    }
}

void PrintSCP::insertTags(QMap<QString, QString>& queryParams, DicomImage *di, QSettings& settings)
{
    auto tagCount = settings.beginReadArray("tag");
    for (int i = 0; i < tagCount; ++i)
    {
        settings.setArrayIndex(i);
        auto key = settings.value("key").toString();

        auto rect = settings.value("rect").toRect();
        if (!rect.isEmpty())
        {
            if (rect.left() < 0) rect.moveLeft(di->getWidth() + rect.left());
            if (rect.top() < 0) rect.moveTop(di->getHeight() + rect.top());
            tess.SetRectangle(rect.left(), rect.top(), rect.width(), rect.height());
        }

        QString str;
        auto pattern = settings.value("pattern").toString();
        if (!pattern.isEmpty())
        {
            str = QString::fromUtf8(tess.GetUTF8Text());
            if (str.isEmpty())
            {
                qDebug() << "No text on the image for idx" << i << "key" << key << "rect" << rect;
            }
            else
            {
                QRegExp re(pattern);
                if (re.indexIn(str) < 0)
                {
                    qDebug() << str << "does not match" << pattern;
                }
                else
                {
                    str = re.cap(1);
                }
            }
        }

        // The pattern is absent or mismatched - send the default value
        //
        if (str.isEmpty())
        {
            str = settings.value("value").toString();
        }

        DcmTag tag;
        if (DcmTag::findTagFromName(key.toUtf8(), tag).bad())
        {
            qDebug() << "Unknown DCM tag" << key;
        }
        else
        {
            auto param = settings.value("query-parameter").toString();
            if (!param.isEmpty())
            {
                queryParams[param] = str;
            }
            qDebug() << tag.getXTag().toString().c_str() << tag.getTagName() << str << param;
            sessionDataset->putAndInsertString(tag, str.toUtf8());
        }
    }
    settings.endArray();
}
