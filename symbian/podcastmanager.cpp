/**
 * This file is part of Podcatcher for N9.
 * Author: Johan Paul (johan.paul@d-pointer.com)
 *
 * Podcatcher for N9 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Podcatcher for N9 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Podcatcher for N9.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkConfiguration>
#include <QNetworkConfigurationManager>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNode>
#include <QImage>
#include <QFile>
#include <QDir>
#include <QMap>

#include <QtDebug>
#include <QCryptographicHash>
#include <QtConcurrentRun>
#include <QFuture>

#include "podcastmanager.h"
#ifdef Q_OS_LINUX
    #include "podcastmanagermeego.h"
#endif

#ifdef Q_OS_SYMBIAN
    #include "podcastmanagersymbian.h"
#endif

#include "podcastsqlmanager.h"
#include "podcastrssparser.h"
#include "podcastglobals.h"

// Initialize the static instance of this class.
PodcastManager* PodcastManager::m_instance = NULL;

PodcastManager::PodcastManager(QObject *parent) :
    QObject(parent),
    m_channelsModel(new PodcastChannelsModel(this)),
    m_networkManager(new QNetworkAccessManager(this)),
    m_dlNetworkManager(new QNetworkAccessManager(this)),
    m_episodeModelFactory(PodcastEpisodesModelFactory::episodesFactory()),
    m_isDownloading(false)
{
    connect(this, SIGNAL(podcastChannelReady(PodcastChannel*)),
            this, SLOT(savePodcastChannel(PodcastChannel*)));
}

PodcastManager * PodcastManager::getInstance(QObject *parent = NULL)
{
    if (m_instance == NULL) {
#ifdef Q_OS_LINUX
        // The Linux Podcast manager
        m_instance = new PodcastManagerMeego(parent);
#endif
#ifdef Q_OS_SYMBIAN
        m_instance = new PodcastManagerSymbian(parent);
#endif
    }

    return m_instance;
}


PodcastChannelsModel * PodcastManager::podcastChannelsModel() const
{
    return m_channelsModel;
}

void PodcastManager::requestPodcastChannel(const QUrl &rssUrl, const QMap<QString, QString> &logoCache)
{
    qDebug() << "Requesting Podcast channel" << rssUrl;

    m_logoCache = logoCache;

    if (!rssUrl.isValid()) {
        qWarning() << "Provided podcast channel URL is not valid.";
        emit showInfoBanner("Unable to add subscription from that location");
        return;
    }

    PodcastChannel *channel = new PodcastChannel(this);
    channel->setUrl(rssUrl.toString());

    if (m_channelsModel->channelAlreadyExists(channel)) {
        qDebug() << "Channel is already in DB. Not doing anything.";
        delete channel;
        emit showInfoBanner("Already subscribed to the channel.");
        return;
    }

    channelRequestMap.insert(rssUrl.toString(), channel);

    QNetworkRequest request;
    request.setUrl(rssUrl);

    QNetworkReply *reply = m_networkManager->get(request);

    connect(reply, SIGNAL(finished()),
            this, SLOT(onPodcastChannelCompleted()));

    m_networkManager->get(request);
}

void PodcastManager::refreshAllChannels()
{
    qDebug() << "\n ********* Refresh episodes for all channels ******** \n";

 //  emit showInfoBanner("Refreshing episodes...");

    emit showInfoDialog("Refreshing episodes...");

    foreach(PodcastChannel *channelIt, m_channelsModel->channels()) {
        qDebug() << "Iterating...";
        int channelid = channelIt->channelDbId();
        PodcastChannel *channel = podcastChannel(channelid);

        qDebug() << "Refreshing channel: " << channelid << channel->title();

        if (channel == 0) {
            qWarning() << "Got NULL episode!";
            break;
        }
        refreshPodcastChannelEpisodes(channel, true);
    }

    emit dismissInfoDialog();
}


void PodcastManager::refreshPodcastChannelEpisodes(PodcastChannel *channel, bool forceNetwork)
{
    qDebug() << "Requesting Podcast channel episodes" << channel->url();
    if (!forceNetwork) {
        // No need to fetch from the net anything.
        savePodcastEpisodes(channel);
        return;
    }

    channel->setIsRefreshing(true);

    qDebug() << "Forced to get new episode data from the network.";

    QUrl rssUrl(channel->url());
    if (!rssUrl.isValid()) {
        qWarning() << "Provided podcast channel URL is not valid.";
        return;
    }

    QNetworkRequest request;
    request.setUrl(rssUrl);

    QNetworkReply *reply = m_networkManager->get(request);

    insertChannelForNetworkReply(reply, channel);

    connect(reply, SIGNAL(finished()),
            this, SLOT(onPodcastEpisodesRequestCompleted()));

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)),
            this, SLOT(onPodcastEpisodesRequestError(QNetworkReply::NetworkError)));
}


PodcastChannel * PodcastManager::podcastChannel(int id)
{
    qDebug() << Q_FUNC_INFO;

    PodcastChannel *channel = NULL;
    if (!m_channelsCache.contains(id)) {
        channel = m_channelsModel->podcastChannelById(id);
        if (channel == 0) {
            return channel;
        }
        m_channelsCache.insert(id, channel);
    } else {
        channel = m_channelsCache.value(id);
    }

    return channel;
}

void PodcastManager::downloadPodcast(PodcastEpisode *episode)
{
    qDebug() << "Episode" << episode->dbid() << "queued for downloading.";

    m_episodeDownloadQueue.append(episode);
    episode->setState(PodcastEpisode::QueuedState);
    executeNextDownload();
}

void PodcastManager::cancelQueueingPodcast(PodcastEpisode *episode)
{
    qDebug() << "Canceling queueing of episode:" << episode->title();

    if (m_episodeDownloadQueue.contains(episode)) {
        m_episodeDownloadQueue.removeOne(episode);
    } else {
        qWarning() << "Canceled episode was not in the queue.";
    }
}

void PodcastManager::cancelDownloadPodcast(PodcastEpisode *episode)
{
    qDebug() << "Canceling download of episode:" << episode->title();

    episode->cancelCurrentDownload();

    if (m_episodeDownloadQueue.contains(episode)) {
        m_episodeDownloadQueue.removeOne(episode);
    } else {
        qWarning() << "Canceled episode was not in the queue.";
    }

    PodcastChannel *channel = m_channelsModel->podcastChannelById(episode->channelid());
    channel->setIsDownloading(false);

    m_isDownloading = false;
    executeNextDownload();
}


/********************************************************************************
 * Private implementations.
 ********************************************************************************/

void PodcastManager::onPodcastChannelCompleted()
{
    qDebug() << "Podcast network request completed.";

    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    QString redirectedUrl = PodcastManager::redirectedRequest(reply);
    if (!redirectedUrl.isEmpty()) {
        requestPodcastChannel(QUrl(redirectedUrl));
        reply->deleteLater();
        return;
    }

    QByteArray data = reply->readAll();
    if (data.size() < 1) {
        qDebug() << "No data in the network reply. Aborting";
        emit showInfoBanner("Unable to add subscription from that location");
        return;
    }

    PodcastChannel *channel = channelRequestMap.value(reply->url().toString());

    QString readyLogoUrl = m_logoCache.value(reply->url().toString());
    if (!readyLogoUrl.isEmpty()) {
        qDebug() << "Got logo from subscription information. Setting it." << readyLogoUrl;
        channel->setLogoUrl(readyLogoUrl);
        m_logoCache.remove(reply->url().toString());
    }

    channel->setXml(data);
    channelRequestMap.remove(reply->url().toString());

    if (PodcastRSSParser::isValidPodcastFeed(data) == false) {
        qDebug() << "Podcast feed is not valid! Not adding data to DB...";
        emit showInfoBanner("Podcast feed is not valid. Cannot add subscription...");
        return;
    }

    bool rssOk;
    rssOk = PodcastRSSParser::populateChannelFromChannelXML(channel,
                                                            channel->xml());
    if (rssOk == false) {
        emit showInfoBanner("Podcast feed is not valid. Cannot add subscription...");
        return;
    }

    // Cache the channel logo locally on file system.
    if (!channel->logoUrl().isEmpty()) {
        QNetworkReply *logoReply = downloadChannelLogo(channel->logoUrl());
        // Use a local map to map data matching this network request to the reply
        // we get.
        insertChannelForNetworkReply(logoReply, channel);
    } else {
        emit podcastChannelReady(channel);
    }

    reply->deleteLater();
}

void PodcastManager::onPodcastChannelLogoCompleted() {

    qDebug() << "Podcast channel logo network request completed";

    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (reply == 0) {
        qWarning() << "Network reply is 0. Aborting.";
        return;
    }

    QString redirectedUrl = redirectedRequest(reply);
    if (!redirectedUrl.isEmpty()) {
        PodcastChannel *channel = channelForNetworkReply(reply);

        QNetworkReply *logoReply = downloadChannelLogo(redirectedUrl);
        reply->deleteLater();

        insertChannelForNetworkReply(logoReply, channel);

        return;
    }

    if (reply->bytesAvailable() < 1) {
        qWarning() << "Got no data from the network request when downloading the logo";
        qDebug() << reply->errorString();
        return;
    }

    PodcastChannel *channel = channelForNetworkReply(reply);
    reply->deleteLater();
    QString channelTitle = channel->title();

    // Use a MD5 hash of the channel name as the logo name that is stored locally.
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(channelTitle.toLatin1());
    QString localFilename = hash.result().toHex();
    qDebug() << "Hash for title " << channelTitle << "=>" << localFilename <<". Using it for cached logo image.";

    // Construct the local QImage from the network data.
    QByteArray imageData = reply->readAll();
    QImage channelLogo = QImage::fromData(imageData);

    QString filename = PODCATCHER_PATH + localFilename + ".jpg";
    qDebug() << "Saving channel logo locally to: " << filename;

    if (channelLogo.isNull()) {
        qWarning() << "Image is NULL";
    }

    if (!channelLogo.save(filename)) {
        qWarning() << "Could not save image: " << filename;
    }

    channel->setLogo(QUrl::fromLocalFile(filename).toString());

    emit podcastChannelReady(channel);
}

void PodcastManager::onPodcastEpisodesRequestCompleted()
{
    qDebug() << "Podcast channel refresh finished";

    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());

    PodcastChannel *channel = channelForNetworkReply(reply);
    if (channel == 0) {
        qWarning() << "Podcast channel from reply is NULL! Doing nothing.";
        return;
    }

    if (reply != 0) {
        QByteArray episodeXmlData = reply->readAll();
        channel->setXml(episodeXmlData);
    }
    reply->deleteLater();

    savePodcastEpisodes(channel);
}

void PodcastManager::onPodcastEpisodesRequestError(QNetworkReply::NetworkError error)
{
    if (error != QNetworkReply::NoError) {
        emit showInfoBanner("Cannot refresh. Network error.");
    }
}

bool PodcastManager::savePodcastEpisodes(PodcastChannel *channel)
{
    QByteArray episodeXmlData = channel->xml();
    QList<PodcastEpisode *> *parsedEpisodes = new QList<PodcastEpisode *>();
    bool rssOk;
    rssOk = PodcastRSSParser::populateEpisodesFromChannelXML(parsedEpisodes,
                                                             episodeXmlData);

    if (rssOk == false) {
        emit showInfoBanner("Podcast feed invalid. Cannot add subscription.");
        return false;
    }

    PodcastEpisodesModel *episodeModel = m_episodeModelFactory->episodesModel(channel->channelDbId());  // FIXME: Pass only channel to episodes model - not the DB id.
    episodeModel->addEpisodes(*parsedEpisodes);

    qDebug() << "Downloading automatically new episodes:" << m_autodownloadOnSettings << " WiFi:" << PodcastManager::isConnectedToWiFi();
    if (m_autodownloadOnSettings && PodcastManager::isConnectedToWiFi()) {
        downloadNewEpisodes(episodeModel->channelId());
    }

    channel->setIsRefreshing(false);

    return true;
}

void PodcastManager::downloadNewEpisodes(int channelId) {
    PodcastEpisodesModel *episodesModel = m_episodeModelFactory->episodesModel(channelId);

    qDebug() << "Downloading new episodes for channel: " << channelId;

    // If the settings value for "get number of episodes" == 0, then we fetch "all episodes". So set 999.
    // Otherwise use the number as specified in the settings.
    int downloadEpisodes = (m_autodownloadNumSettings == 0) ? 999 : m_autodownloadNumSettings;
    QList<PodcastEpisode *> episodes = episodesModel->undownloadedEpisodes(downloadEpisodes);
    foreach(PodcastEpisode *episode, episodes) {
        qDebug() << "Downloading podcast:" << episode->downloadLink();
        downloadPodcast(episode);
    }
}


void PodcastManager::savePodcastChannel(PodcastChannel *channel)
{
    qDebug() << "Adding channel to DB:" << channel->title();
    m_channelsModel->addChannel(channel);

    qDebug() << "Podcast channel saved to DB. Refreshing episodes...";
    refreshPodcastChannelEpisodes(channel);

    emit podcastChannelSaved();
}

void PodcastManager::onPodcastEpisodeDownloaded(PodcastEpisode *episode)
{
    qDebug() << "Download completed...";

    disconnect(episode, SIGNAL(podcastEpisodeDownloaded(PodcastEpisode*)),
            this, SLOT(onPodcastEpisodeDownloaded(PodcastEpisode*)));

    disconnect(episode, SIGNAL(podcastEpisodeDownloadFailed(PodcastEpisode*)),
            this, SLOT(onPodcastEpisodeDownloadFailed(PodcastEpisode*)));

    episode->setState(PodcastEpisode::DownloadedState);

    PodcastEpisodesModel *episodeModel = m_episodeModelFactory->episodesModel(episode->channelid());
    episodeModel->refreshEpisode(episode);
    m_channelsModel->refreshChannel(episode->channelid());

    PodcastChannel *channel = m_channelsModel->podcastChannelById(episode->channelid());
    channel->setIsDownloading(false);

    emit podcastEpisodeDownloaded(episode);

    m_isDownloading = false;

    if (m_episodeDownloadQueue.contains(episode)) {
        m_episodeDownloadQueue.removeOne(episode);
    }

    executeNextDownload();
}

void PodcastManager::onPodcastEpisodeDownloadFailed(PodcastEpisode *episode)
{
    qDebug() << "Download failed...";

    disconnect(episode, SIGNAL(podcastEpisodeDownloaded(PodcastEpisode*)),
            this, SLOT(onPodcastEpisodeDownloaded(PodcastEpisode*)));

    disconnect(episode, SIGNAL(podcastEpisodeDownloadFailed(PodcastEpisode*)),
            this, SLOT(onPodcastEpisodeDownloadFailed(PodcastEpisode*)));

    m_isDownloading = false;
    PodcastChannel *channel = m_channelsModel->podcastChannelById(episode->channelid());
    channel->setIsDownloading(false);


    if (m_episodeDownloadQueue.contains(episode)) {
        m_episodeDownloadQueue.removeOne(episode);
    }

    executeNextDownload();
}

void PodcastManager::executeNextDownload()
{
    if (!m_isDownloading && !m_episodeDownloadQueue.isEmpty()) {
        emit downloadingPodcasts(true);  // Notify the UI

        m_isDownloading = true;
        PodcastEpisode *episode = m_episodeDownloadQueue.first();

        qDebug() << "Starting a new download..." << episode->title();

        connect(episode, SIGNAL(podcastEpisodeDownloaded(PodcastEpisode*)),
                this, SLOT(onPodcastEpisodeDownloaded(PodcastEpisode*)));

        connect(episode, SIGNAL(podcastEpisodeDownloadFailed(PodcastEpisode*)),
                this, SLOT(onPodcastEpisodeDownloadFailed(PodcastEpisode*)));

        PodcastChannel *channel = m_channelsModel->podcastChannelById(episode->channelid());
        channel->setIsDownloading(true);

        episode->setState(PodcastEpisode::DownloadingState);
        episode->setHasBeenCanceled(false);
        episode->setDownloadManager(m_dlNetworkManager);
        episode->downloadEpisode();
    } else {
        if (m_episodeDownloadQueue.isEmpty()) {
            emit downloadingPodcasts(false); // Notify the UI
        }
    }
}

QString PodcastManager::redirectedRequest(QNetworkReply *reply)
{
    QVariant possibleRedirectUrl =
                     reply->attribute(QNetworkRequest::RedirectionTargetAttribute);

    if (possibleRedirectUrl.toUrl().isValid()) {
        QUrl redirectedUrl = QUrl::fromUserInput(possibleRedirectUrl.toString());
        qDebug() << "We have been redirected. New URL is " << redirectedUrl;
        return redirectedUrl.toString();
    }

    return QString();
}

QNetworkReply * PodcastManager::downloadChannelLogo(QString logoUrl)
{
    QNetworkRequest r;
    r.setUrl(QUrl(logoUrl));

    QNetworkReply *logoReply = m_networkManager->get(r);

    connect(logoReply, SIGNAL(finished()),
            this, SLOT(onPodcastChannelLogoCompleted()));

    return logoReply;
}

void PodcastManager::insertChannelForNetworkReply(QNetworkReply *reply, PodcastChannel *channel)
{
    if (reply == 0) {
        qWarning() << "Network reply is NULL. Inserting nothing.";
    }

    if (channel == 0) {
        qWarning() << "Channel is NULL. Inserting nothing.";
    }

    m_channelNetworkRequestCache.insert(reply, channel);
}

PodcastChannel * PodcastManager::channelForNetworkReply(QNetworkReply *reply)
{
    PodcastChannel *channel = 0;
    if (reply == 0) {
        return channel;
    }

    if (m_channelNetworkRequestCache.isEmpty()) {
        qWarning() << "Podcast channel network cache is empty. Returning NULL channel!";
        return channel;
    }

    if (!m_channelNetworkRequestCache.contains(reply)) {
        qWarning() << "Podcast channel network cache does not contain network reply. Returning NULL nhannel!";
        return channel;
    }

    channel = m_channelNetworkRequestCache.value(reply);
    m_channelNetworkRequestCache.remove(reply);

    return channel;
}

bool PodcastManager::isConnectedToWiFi()
{
    QNetworkConfigurationManager mgr;
    QList<QNetworkConfiguration> confs = mgr.allConfigurations(QNetworkConfiguration::Active);
    foreach(const QNetworkConfiguration &netconf, confs) {
        if (netconf.bearerType() == QNetworkConfiguration::BearerWLAN) {
            qDebug() << "We are connected to a WiFi network.";
            return true;
        }
    }

    qDebug() << "We are NOT connected to a WiFI network.";
    return false;
}

void PodcastManager::removePodcastChannel(int channelId)
{
    /**
     * Delete episode data.
     */
    // Fetch model from model factory - then delete it from the factory's cache.
    PodcastEpisodesModel *episodesModel = m_episodeModelFactory->episodesModel(channelId);
    QList<PodcastEpisode *> episodes = episodesModel->episodes();

    // Se if any episodes are being downloaded from this channel. Remove them from queue.
    foreach(PodcastEpisode* episode, episodes) {
        if (m_episodeDownloadQueue.contains(episode)) {
            onPodcastEpisodeDownloadFailed(episode);
            m_episodeDownloadQueue.removeOne(episode);  // removeOne: There ought to be always just one. Faster.
        }
    }

    // This will also call episodes->deleteDownload(); for all episodes in the model.
    m_episodeModelFactory->removeFromCache(channelId);
    episodesModel->removeAll();
    delete episodesModel;

    /**
     * Delete channel data.
     * Do not touch the episode anymore!
     */
    // Deleting locally cached channel logo.
    PodcastChannel *channel = m_channelsCache.value(channelId);

    QUrl channelLogoUrl(channel->logo());
    QFile channelLogo(channelLogoUrl.toLocalFile());
    if (!channelLogo.remove()) {
        QFileInfo fi(channelLogo);
        qWarning() << "Could not remove cached logo for channel:" << channel->title() << fi.absoluteFilePath();
    }

    // Finally remove the channel from the model and the cache.
    m_channelsModel->removeChannel(channel);
    m_channelsCache.remove(channelId);

    // Finally delete the memory reserved for the channel
    delete channel;
}

void PodcastManager::deleteAllDownloadedPodcasts(int channelId)
{
    PodcastEpisodesModel *episodesModel = m_episodeModelFactory->episodesModel(channelId);
    QList<PodcastEpisode *> episodes = episodesModel->episodes();

    foreach(PodcastEpisode *episode, episodes) {
        if (!episode->playFilename().isEmpty()) {
            episode->deleteDownload();
            episodesModel->refreshEpisode(episode);
        }
    }
}

bool PodcastManager::isDownloading()
{
    return m_isDownloading;
}

