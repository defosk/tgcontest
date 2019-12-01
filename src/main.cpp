#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include <boost/program_options.hpp>
#include <fasttext.h>
#include <nlohmann_json/json.hpp>

#include "clustering/dbscan.h"
#include "clustering/slink.h"
#include "clustering/in_cluster_ranging.h"
#include "detect.h"
#include "parser.h"
#include "timer.h"
#include "util.h"

namespace po = boost::program_options;

int main(int argc, char** argv) {
    try {
        po::options_description desc("options");
        desc.add_options()
            ("mode", po::value<std::string>()->required(), "mode")
            ("source_dir", po::value<std::string>()->required(), "source_dir")
            ("lang_detect_model", po::value<std::string>()->default_value("models/lang_detect.ftz"), "lang_detect_model")
            ("news_detect_model", po::value<std::string>()->default_value("models/news_detect.ftz"), "news_detect_model")
            ("ru_cat_detect_model", po::value<std::string>()->default_value("models/ru_cat_detect.ftz"), "ru_cat_detect_model")
            ("en_cat_detect_model", po::value<std::string>()->default_value("models/en_cat_detect.ftz"), "en_cat_detect_model")
            ("ru_vector_model", po::value<std::string>()->default_value("models/ru_tg_lenta_vector_model.bin"), "ru_vector_model")
            ("en_vector_model", po::value<std::string>()->default_value("models/en_tg_bbc_nc_vector_model.bin"), "en_vector_model")
            ("clustering_type", po::value<std::string>()->default_value("slink"), "clustering_type")
            ("clustering_distance_threshold", po::value<float>()->default_value(0.05f), "clustering_distance_threshold")
            ("clustering_eps", po::value<double>()->default_value(0.3), "clustering_eps")
            ("clustering_min_points", po::value<size_t>()->default_value(1), "clustering_min_points")
            ("en_rating", po::value<std::string>()->default_value("ratings/en_rating.txt"), "en_rating")
            ("ru_rating", po::value<std::string>()->default_value("ratings/ru_rating.txt"), "ru_rating")
            ("ndocs", po::value<int>()->default_value(-1), "ndocs")
            ("languages", po::value<std::vector<std::string>>()->multitoken()->default_value(std::vector<std::string>{"ru", "en"}, "ru en"), "languages")
            ;

        po::positional_options_description p;
        p.add("mode", 1);
        p.add("source_dir", 1);

        po::command_line_parser parser{argc, argv};
        parser.options(desc).positional(p);
        po::parsed_options parsed_options = parser.run();
        po::variables_map vm;
        po::store(parsed_options, vm);
        po::notify(vm);

        // Args check
        if (!vm.count("mode") || !vm.count("source_dir")) {
            std::cerr << "Not enough arguments" << std::endl;
            return -1;
        }
        std::string mode = vm["mode"].as<std::string>();
        std::cerr << "Mode: " << mode << std::endl;
        std::vector<std::string> modes = {
            "languages",
            "news",
            "sites",
            "json",
            "toloka",
            "categories",
            "threads"
        };
        if (std::find(modes.begin(), modes.end(), mode) == modes.end()) {
            std::cerr << "Unknown or unsupported mode!" << std::endl;
            return -1;
        }

        // Load models
        std::cerr << "Loading models..." << std::endl;
        std::vector<std::string> modelsOptions = {
            "lang_detect_model",
            "news_detect_model",
            "ru_cat_detect_model",
            "en_cat_detect_model",
            "ru_vector_model",
            "en_vector_model"
        };
        std::unordered_map<std::string, std::unique_ptr<fasttext::FastText>>models;
        for (const auto& optionName : modelsOptions) {
            const std::string modelPath = vm[optionName].as<std::string>();
            std::unique_ptr<fasttext::FastText> model(new fasttext::FastText());
            models.emplace(optionName, std::move(model));
            models.at(optionName)->loadModel(modelPath);
            std::cerr << "FastText " << optionName << " loaded" << std::endl;
        }

        // Load agency ratings
        std::cerr << "Loading agency ratings..." << std::endl;
        std::vector<std::string> ratingFiles = {vm["en_rating"].as<std::string>(), vm["ru_rating"].as<std::string>()};;
        std::unordered_map<std::string, double> agencyRating = LoadRatings(ratingFiles);
        std::cerr << "Agency ratings loaded" << std::endl;

        // Read file names
        std::cerr << "Reading file names..." << std::endl;
        std::string sourceDir = vm["source_dir"].as<std::string>();
        int nDocs = vm["ndocs"].as<int>();
        std::vector<std::string> fileNames;
        ReadFileNames(sourceDir, fileNames, nDocs);
        std::cerr << "Files count: " << fileNames.size() << std::endl;

        // Parse files and filter by language
        std::vector<std::string> languages = vm["languages"].as<std::vector<std::string>>();
        std::cerr << "Parsing " << fileNames.size() << " files..." << std::endl;
        std::vector<Document> docs;
        docs.reserve(fileNames.size() / 2);
        const auto& langDetectModel = *models.at("lang_detect_model");
        for (const std::string& path: fileNames) {
            Document doc = ParseFile(path.c_str());
            doc.Language = DetectLanguage(langDetectModel, doc);
            if (std::find(languages.begin(), languages.end(), doc.Language) != languages.end()) {
                doc.IsNews = DetectIsNews(*models.at("news_detect_model"), doc);
                doc.Category = DetectCategory(*models.at(doc.Language + "_cat_detect_model"), doc);
                docs.push_back(doc);
            }
        }
        docs.shrink_to_fit();
        std::cerr << docs.size() << " documents saved" << std::endl;

        // Output
        if (mode == "languages") {
            nlohmann::json outputJson = nlohmann::json::array();
            std::map<std::string, std::vector<std::string>> langToFiles;
            for (const Document& doc : docs) {
                langToFiles[doc.Language].push_back(GetCleanFileName(doc.FileName));
            }
            for (const auto& pair : langToFiles) {
                const std::string& language = pair.first;
                const std::vector<std::string>& files = pair.second;
                nlohmann::json object = {
                    {"lang_code", language},
                    {"articles", files}
                };
                outputJson.push_back(object);
            }
            std::cout << outputJson.dump(4) << std::endl;
        } else if (mode == "sites") {
            nlohmann::json outputJson = nlohmann::json::array();
            std::map<std::string, std::vector<std::string>> siteToTitles;
            for (const Document& doc : docs) {
                siteToTitles[doc.SiteName].push_back(doc.Title);
            }
            for (const auto& pair : siteToTitles) {
                const std::string& site = pair.first;
                const std::vector<std::string>& titles = pair.second;
                nlohmann::json object = {
                    {"site", site},
                    {"titles", titles}
                };
                outputJson.push_back(object);
            }
            std::cout << outputJson.dump(4) << std::endl;
        } else if (mode == "json") {
            nlohmann::json outputJson = nlohmann::json::array();
            for (const Document& doc : docs) {
                nlohmann::json object = {
                    {"url", doc.Url},
                    {"site_name", doc.SiteName},
                    {"date", doc.DateTime},
                    {"title", doc.Title},
                    {"description", doc.Description},
                    {"text", doc.Text},
                    {"out_links", doc.OutLinks},
                    {"language", doc.Language},
                    {"category", doc.Category},
                    {"is_news", doc.IsNews}
                };
                outputJson.push_back(object);
            }
            std::cout << outputJson.dump(4) << std::endl;
        } else if (mode == "news") {
            nlohmann::json articles = nlohmann::json::array();
            for (const Document& doc : docs) {
                if (!doc.IsNews) {
                    continue;
                }
                articles.push_back(GetCleanFileName(doc.FileName));
            }
            nlohmann::json outputJson = nlohmann::json::object();
            outputJson["articles"] = articles;
            std::cout << outputJson.dump(4) << std::endl;
        } else if (mode == "categories") {
            nlohmann::json outputJson = nlohmann::json::array();
            std::map<std::string, std::vector<std::string>> catToFiles;
            for (const Document& doc : docs) {
                if (!doc.IsNews || doc.Category == "not_news") {
                    continue;
                }
                catToFiles[doc.Category].push_back(GetCleanFileName(doc.FileName));
            }
            for (const auto& pair : catToFiles) {
                const std::string& category = pair.first;
                const std::vector<std::string>& files = pair.second;
                nlohmann::json object = {
                    {"category", category},
                    {"articles", files}
                };
                outputJson.push_back(object);
            }
            std::cout << outputJson.dump(4) << std::endl;
        } else if (mode == "threads") {
            std::unique_ptr<Clustering> ruClustering;
            std::unique_ptr<Clustering> enClustering;

            const std::string clusteringType = vm["clustering_type"].as<std::string>();
            if (clusteringType == "slink") {
                const float distanceThreshold = vm["clustering_distance_threshold"].as<float>();
                ruClustering = std::unique_ptr<Clustering>(new SlinkClustering(*models.at("ru_vector_model"), distanceThreshold));
                enClustering = std::unique_ptr<Clustering>(new SlinkClustering(*models.at("en_vector_model"), distanceThreshold));
            }
            else if (clusteringType == "dbscan") {
                const double eps = vm["clustering_eps"].as<double>();
                const size_t minPoints = vm["clustering_min_points"].as<size_t>();
                ruClustering = std::unique_ptr<Clustering>(new Dbscan(*models.at("ru_vector_model"), eps, minPoints));
                enClustering = std::unique_ptr<Clustering>(new Dbscan(*models.at("en_vector_model"), eps, minPoints));
            }

            Timer<std::chrono::high_resolution_clock, std::chrono::milliseconds> timer;

            std::vector<Document> ruDocs;
            std::vector<Document> enDocs;
            while (!docs.empty()) {
                const Document& doc = docs.back();
                if (!doc.IsNews || doc.Category == "not_news") {
                    docs.pop_back();
                    continue;
                }
                if (doc.Language == "en") {
                    enDocs.push_back(doc);
                } else if (doc.Language == "ru") {
                    ruDocs.push_back(doc);
                }
                docs.pop_back();
            }
            docs.shrink_to_fit();
            docs.clear();

            const Clustering::Clusters ruClusters = ruClustering->Cluster(ruDocs);
            const Clustering::Clusters enClusters = enClustering->Cluster(enDocs);
            std::cout << "CLUSTERING: " << timer.Elapsed() << " ms (" << ruClusters.size() << "clusters)" << std::endl;

            const auto ruClustersSummarized = InClusterRanging(ruClusters, agencyRating);
            for (const auto& cluster : ruClustersSummarized) {
                if (cluster.size() < 2) {
                    continue;
                }
                std::cout << "CLUSTER" << std::endl;
                for (const auto& doc : cluster) {
                    std::cout << "   " << doc.get().Title << " (" << doc.get().Url << ")" << std::endl;
                }
            }
        }
        return 0;
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return -1;
    }
}
