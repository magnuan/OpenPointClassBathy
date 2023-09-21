#include <random>
#include <filesystem>

#include "point_io.hpp"
#include "labels.hpp"

namespace fs = std::filesystem;

double PointSet::spacing(int kNeighbors) {
    if (m_spacing != -1) return m_spacing;

    const auto index = getIndex<KdTree>();

    const size_t np = count();
    const size_t SAMPLES = std::min<size_t>(np, 10000);
    const int count = kNeighbors + 1;

    std::unordered_map<size_t, size_t> dist_map;

    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<size_t> randomDis(
        std::numeric_limits<size_t>::min(),
        np - 1
    );

    std::vector<size_t> indices(count);
    std::vector<float> sqr_dists(count);

    //For up to 10k random points in dataset, 
    for (size_t i = 0; i < SAMPLES; ++i) {
        const size_t idx = randomDis(gen);
        // Find count nearest neighbours, and their squared distance
        index->knnSearch(points[idx].data(), count, indices.data(), sqr_dists.data());
        //Calcualte RMS distance to the count nearest neighbours
        float sum = 0.0;
        for (size_t j = 1; j < kNeighbors; ++j) {
            sum += std::sqrt(sqr_dists[j]);
        }
        sum /= static_cast<float>(kNeighbors);

        auto k = static_cast<size_t>(std::ceil(sum * 100));
        
        /*Add 100 x distance to RMS distance "heatmap" 
        * so:
        *  <= 1cm is index 1
        *  1-2cm  is index 2
        *  2-3cm is index 3
        * etc
        */
        if (dist_map.find(k) == dist_map.end()) {
            dist_map[k] = 1;
        }
        else {
            dist_map[k] += 1;
        }
    }
    
    // Find most frequent value in RMS distance heatmap
    size_t max_val = std::numeric_limits<size_t>::min();
    size_t d = 0;
    for (const auto it : dist_map) {
        if (it.second > max_val) {
            d = it.first;
            max_val = it.second;
        }
    }
    //Scale down to real value again and return
    m_spacing = std::max(0.01, static_cast<double>(d) / 100.0);
    return m_spacing;
}

std::string getVertexLine(std::ifstream &reader) {
    std::string line;

    // Skip comments
    do {
        std::getline(reader, line);

        if (line.find("element") == 0)
            return line;
        else if (line.find("comment") == 0)
            continue;
        else
            throw std::runtime_error("Invalid PLY file");
    } while (true);
}

size_t getVertexCount(const std::string &line) {

    // Split line into tokens
    std::vector<std::string> tokens;

    std::istringstream iss(line);
    std::string token;
    while (std::getline(iss, token, ' '))
        tokens.push_back(token);

    if (tokens.size() < 3)
        throw std::runtime_error("Invalid PLY file");

    if (tokens[0] != "element" && tokens[1] != "vertex")
        throw std::runtime_error("Invalid PLY file");

    return std::stoi(tokens[2]);
}

PointSet *readPointSet(const std::string &filename) {
    PointSet *r;
    const fs::path p(filename);
    if (p.extension().string() == ".ply") r = fastPlyReadPointSet(filename);
    else r = pdalReadPointSet(filename);

    // Re-map labels if needed
    if (r->hasLabels()) {
        auto mappings = getClassMappings(filename);
        const bool hasMappings = !mappings.empty();

        if (hasMappings) {
            auto trainingCodes = getTrainingCodes();
            for (size_t idx = 0; idx < r->count(); idx++) {
                int label = r->labels[idx];

                if (mappings.find(label) != mappings.end()) {
                    label = trainingCodes[mappings[label]];
                }
                else {
                    label = trainingCodes["unassigned"];
                }

                r->labels[idx] = label;
            }
        }
        else {
            auto asprs2TrainCodes = getAsprs2TrainCodes();
            for (size_t idx = 0; idx < r->count(); idx++) {
                int label = r->labels[idx];
                r->labels[idx] = asprs2TrainCodes[label];
            }
        }
    }

    return r;
}

PointSet *fastPlyReadPointSet(const std::string &filename) {
    std::ifstream reader(filename, std::ios::binary);
    if (!reader.is_open())
        throw std::runtime_error("Cannot open file " + filename);

    auto *r = new PointSet();

    std::string line;
    std::getline(reader, line);
    line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());

    if (line != "ply")
        throw std::runtime_error("Invalid PLY file (header does not start with ply)");

    std::getline(reader, line);
    line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());

    // We are reading an ascii ply
    bool ascii = line == "format ascii 1.0";

    const auto vertexLine = getVertexLine(reader);
    const auto count = getVertexCount(vertexLine);

    std::cout << "Reading " << count << " points" << std::endl;

    checkHeader(reader, "x");
    checkHeader(reader, "y");
    checkHeader(reader, "z");

    int c = 0;
    bool hasViews = false;
    bool hasNormals = false;
    bool hasColors = false;
    std::string labelDim;

    size_t redIdx = 0, greenIdx = 1, blueIdx = 2;

    std::getline(reader, line);
    line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());

    while (line != "end_header") {
        if (hasHeader(line, "nx") || hasHeader(line, "normal_x") || hasHeader(line, "normalx")) hasNormals = true;
        if (hasHeader(line, "red")) {
            hasColors = true;
            redIdx = c;
        }
        if (hasHeader(line, "green")) {
            hasColors = true;
            greenIdx = c;
        }
        if (hasHeader(line, "blue")) {
            hasColors = true;
            blueIdx = c;
        }
        if (hasHeader(line, "views")) hasViews = true;

        if (hasHeader(line, "label")) labelDim = "label";
        if (hasHeader(line, "classification")) labelDim = "classification";
        if (hasHeader(line, "class")) labelDim = "class";

        if (c++ > 100) break;
        std::getline(reader, line);
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
    }

    size_t colorIdxMin = std::min<size_t>(std::min<size_t>(redIdx, greenIdx), blueIdx);
    redIdx -= colorIdxMin;
    greenIdx -= colorIdxMin;
    blueIdx -= colorIdxMin;
    if (redIdx + greenIdx + blueIdx != 3) throw std::runtime_error("red/green/blue properties need to be contiguous");

    bool hasLabels = !labelDim.empty();

    r->points.resize(count);
    if (hasNormals) r->normals.resize(count);
    if (hasColors) r->colors.resize(count);
    if (hasViews) r->views.resize(count);
    if (hasLabels) r->labels.resize(count);

    // if (hasNormals) std::cout << "N";
    // if (hasColors) std::cout << "C";
    // if (hasViews) std::cout << "V";
    // if (hasLabels) std::cout << "L";
    // std::cout << std::endl;

    // Read points
    if (ascii) {
        uint16_t buf;

        for (size_t i = 0; i < count; i++) {
            reader >> r->points[i][0]
                >> r->points[i][1]
                >> r->points[i][2];
            if (hasNormals) {
                reader >> r->normals[i][0]
                    >> r->normals[i][1]
                    >> r->normals[i][2];
            }
            if (hasColors) {
                reader >> buf;
                r->colors[i][redIdx] = static_cast<uint8_t>(buf);
                reader >> buf;
                r->colors[i][greenIdx] = static_cast<uint8_t>(buf);
                reader >> buf;
                r->colors[i][blueIdx] = static_cast<uint8_t>(buf);
            }
            if (hasViews) {
                reader >> buf;
                r->views[i] = static_cast<uint8_t>(buf);
            }
            if (hasLabels) {
                reader >> buf;
                r->labels[i] = static_cast<uint8_t>(buf);
            }
        }
    }
    else {

        // Read points
        uint8_t color[3];

        for (size_t i = 0; i < count; i++) {
            reader.read(reinterpret_cast<char *>(r->points[i].data()), sizeof(float) * 3);

            if (hasNormals) {
                reader.read(reinterpret_cast<char *>(r->normals[i].data()), sizeof(float) * 3);
            }

            if (hasColors) {
                reader.read(reinterpret_cast<char *>(&color), sizeof(uint8_t) * 3);
                r->colors[i][redIdx] = color[0];
                r->colors[i][greenIdx] = color[1];
                r->colors[i][blueIdx] = color[2];
            }

            if (hasViews) {
                reader.read(reinterpret_cast<char *>(&r->views[i]), sizeof(uint8_t));
            }

            if (hasLabels) {
                reader.read(reinterpret_cast<char *>(&r->labels[i]), sizeof(uint8_t));
            }
        }
    }

    // std::vector<size_t> classes(255, 0);
    // for (size_t idx = 0; idx < count; idx++) {
    //     std::cout << r->points[idx][0] << " ";
    //     std::cout << r->points[idx][1] << " ";
    //     std::cout << r->points[idx][2] << " ";

    //     std::cout << std::to_string(r->colors[idx][0]) << " ";
    //     std::cout << std::to_string(r->colors[idx][1]) << " ";
    //     std::cout << std::to_string(r->colors[idx][2]) << " ";

    //     if (hasLabels){
    //         std::cout << std::to_string(r->labels[idx]) << " ";
    //     }
    //     std::cout << std::endl;

    //     if (idx > 9) exit(1);
    //     classes[std::size_t(r->labels[idx])]++;
    // }

    // for (size_t i = 0; i < classes.size(); i++){
    //     std::cout << i << ": " << classes[i] << std::endl;
    // }
    // exit(1);

    reader.close();

    return r;
}

PointSet *pdalReadPointSet(const std::string &filename) {
    #ifdef WITH_PDAL
    std::string labelDimension;
    pdal::StageFactory factory;
    const std::string driver = pdal::StageFactory::inferReaderDriver(filename);
    if (driver.empty()) {
        throw std::runtime_error("Can't infer point cloud reader from " + filename);
    }

    auto *r = new PointSet();
    pdal::Stage *s = factory.createStage(driver);
    pdal::Options opts;
    opts.add("filename", filename);
    s->setOptions(opts);

    auto *table = new pdal::PointTable();

    std::cout << "Reading points from " << filename << std::endl;

    s->prepare(*table);
    const pdal::PointViewSet pvSet = s->execute(*table);

    r->pointView = *pvSet.begin();
    const pdal::PointViewPtr pView = r->pointView;

    if (pView->empty()) {
        throw std::runtime_error("No points could be fetched");
    }

    std::cout << "Number of points: " << pView->size() << std::endl;

    for (const auto &d : pView->dims()) {
        std::string dim = pView->dimName(d);
        if (dim == "Label" || dim == "label" ||
            dim == "Classification" || dim == "classification" ||
            dim == "Class" || dim == "class") {
            labelDimension = dim;
        }
    }

    const size_t count = pView->size();
    const pdal::PointLayoutPtr layout(table->layout());
    const bool hasLabels = !labelDimension.empty();

    pdal::Dimension::Id labelId;
    if (hasLabels) {
        std::cout << "Label dimension: " << labelDimension << std::endl;
        labelId = layout->findDim(labelDimension);
        r->labels.resize(count);
    }

    r->points.resize(count);
    bool hasColors = false;
    bool largeColors = false;

    if (layout->hasDim(pdal::Dimension::Id::Green)) {
        r->colors.resize(count);
        hasColors = true;
        for (pdal::PointId idx = 0; idx < count; ++idx) {
            if (pView->getFieldAs<uint16_t>(pdal::Dimension::Id::Green, idx) > 255) {
                largeColors = true;
                break;
            }
        }
    }

    for (pdal::PointId idx = 0; idx < count; ++idx) {
        auto p = pView->point(idx);
        r->points[idx][0] = p.getFieldAs<float>(pdal::Dimension::Id::X);
        r->points[idx][1] = p.getFieldAs<float>(pdal::Dimension::Id::Y);
        r->points[idx][2] = p.getFieldAs<float>(pdal::Dimension::Id::Z);

        if (hasColors) {
            if (largeColors) {
                r->colors[idx][0] = static_cast<uint8_t>((p.getFieldAs<double>(pdal::Dimension::Id::Red) / 65535.0) * 255.0);
                r->colors[idx][1] = static_cast<uint8_t>((p.getFieldAs<double>(pdal::Dimension::Id::Green) / 65535.0) * 255.0);
                r->colors[idx][2] = static_cast<uint8_t>((p.getFieldAs<double>(pdal::Dimension::Id::Blue) / 65535.0) * 255.0);
            }
            else {
                r->colors[idx][0] = p.getFieldAs<uint8_t>(pdal::Dimension::Id::Red);
                r->colors[idx][1] = p.getFieldAs<uint8_t>(pdal::Dimension::Id::Green);
                r->colors[idx][2] = p.getFieldAs<uint8_t>(pdal::Dimension::Id::Blue);
            }
        }

        if (hasLabels) {
            r->labels[idx] = p.getFieldAs<uint8_t>(labelId);
        }
    }

    // std::vector<std::size_t> classes (255, 0);
    // for (size_t idx = 0; idx < count; idx++) {
        // std::cout << r->points[idx][0] << " ";
        // std::cout << r->points[idx][1] << " ";
        // std::cout << r->points[idx][2] << " ";

        // std::cout << std::to_string(r->colors[idx][0]) << " ";
        // std::cout << std::to_string(r->colors[idx][1]) << " ";
        // std::cout << std::to_string(r->colors[idx][2]) << " ";

        // if (hasLabels){
        //     std::cout << std::to_string(r->labels[idx]) << " ";
        // }
        // std::cout << std::endl;

        // if (idx > 9) exit(1);

    //     classes[std::size_t(r->labels[idx])]++;
    // }

    // for (size_t i = 0; i < classes.size(); i++){
    //     std::cout << i << ": " << classes[i] << std::endl;
    // }
    // exit(1);

    return r;
    #else
    fs::path p(filename);
    throw std::runtime_error("Unsupported file extension " + p.extension().string() + ", build program with PDAL support for additional file types support.");
    #endif
}

void checkHeader(std::ifstream &reader, const std::string &prop) {
    std::string line;
    std::getline(reader, line);
    line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());

    if (line.substr(line.length() - prop.length(), prop.length()) != prop) {
        throw std::runtime_error("Invalid PLY file (expected 'property * " + prop + "', but found '" + line + "')");
    }
}

bool hasHeader(const std::string &line, const std::string &prop) {
    //std::cout << line << " -> " << prop << " : " << line.substr(line.length() - prop.length(), prop.length()) << std::endl;
    return line.substr(0, 8) == "property" && line.substr(line.length() - prop.length(), prop.length()) == prop;
}

void savePointSet(PointSet &pSet, const std::string &filename) {
    
    std::cout<< "savePointSet" << std::endl;
    for (size_t idx = 0; idx < pSet.count(); idx++) {
        int label = pSet.labels[idx];
    }

    const fs::path p(filename);
    if (p.extension().string() == ".ply") fastPlySavePointSet(pSet, filename);
    else pdalSavePointSet(pSet, filename);
}

void pdalSavePointSet(PointSet &pSet, const std::string &filename) {
    #ifdef WITH_PDAL
    pdal::StageFactory factory;
    const std::string driver = pdal::StageFactory::inferWriterDriver(filename);
    if (driver.empty()) {
        throw std::runtime_error("Can't infer point cloud writer from " + filename);
    }

    // Sync position, color and label data
    if (pSet.pointView == nullptr) throw std::runtime_error("pointView is null (should not have happened)");
    const pdal::PointViewPtr pView = pSet.pointView;

    for (pdal::PointId idx = 0; idx < pSet.count(); idx++) {
        if (pSet.hasColors()) {
            pView->setField(pdal::Dimension::Id::Red, idx, pSet.colors[idx][0]);
            pView->setField(pdal::Dimension::Id::Green, idx, pSet.colors[idx][1]);
            pView->setField(pdal::Dimension::Id::Blue, idx, pSet.colors[idx][2]);
        }
        
        int label = pSet.labels[idx];
        //std::cout << "PDAL point  " << idx << " label is " << label << std::endl;

        if (pSet.hasLabels()) {
            pView->setField(pdal::Dimension::Id::Classification, idx, label);
        }
    }

    pdal::PointTable table;
    pdal::BufferReader reader;
    reader.addView(pView);

    for (const auto d : pView->dims()) {
        table.layout()->registerOrAssignDim(pView->dimName(d), pView->dimType(d));
    }

    pdal::Stage *s = factory.createStage(driver);
    pdal::Options opts;
    opts.add("filename", filename);
    s->setOptions(opts);
    s->setInput(reader);

    s->prepare(table);
    s->execute(table);

    std::cout << "PDAL wrote " << filename << std::endl;
    #else
    fs::path p(filename);
    throw std::runtime_error("Unsupported file extension " + p.extension().string() + ", build program with PDAL support for additional file types support.");
    #endif
}

void fastPlySavePointSet(PointSet &pSet, const std::string &filename) {
    std::ofstream o(filename, std::ios::binary);

    o << "ply" << std::endl;
    o << "format binary_little_endian 1.0" << std::endl;
    o << "comment Generated by OpenPointClass" << std::endl;
    o << "element vertex " << pSet.count() << std::endl;
    o << "property float x" << std::endl;
    o << "property float y" << std::endl;
    o << "property float z" << std::endl;

    const bool hasNormals = pSet.hasNormals();
    const bool hasColors = pSet.hasColors();
    const bool hasViews = pSet.hasViews();
    const bool hasLabels = pSet.hasLabels();

    if (hasNormals) {
        o << "property float nx" << std::endl;
        o << "property float ny" << std::endl;
        o << "property float nz" << std::endl;
    }
    if (hasColors) {
        o << "property uchar red" << std::endl;
        o << "property uchar green" << std::endl;
        o << "property uchar blue" << std::endl;
    }
    if (hasViews) {
        o << "property uchar views" << std::endl;
    }
    if (hasLabels) {
        o << "property uchar classification" << std::endl;
    }

    o << "end_header" << std::endl;

    for (size_t i = 0; i < pSet.count(); i++) {
        o.write(reinterpret_cast<const char *>(pSet.points[i].data()), sizeof(float) * 3);
        if (hasNormals) o.write(reinterpret_cast<const char *>(pSet.normals[i].data()), sizeof(float) * 3);
        if (hasColors) o.write(reinterpret_cast<const char *>(pSet.colors[i].data()), sizeof(uint8_t) * 3);
        if (hasViews) o.write(reinterpret_cast<const char *>(&pSet.views[i]), sizeof(uint8_t));
        if (hasLabels) o.write(reinterpret_cast<const char *>(&pSet.labels[i]), sizeof(uint8_t));
    }

    o.close();
    std::cout << "Wrote " << filename << std::endl;
}

std::unordered_map<int, std::string> getClassMappings(const std::string &filename) {
    fs::path p(filename);
    std::string jsonFile = p.replace_filename(p.filename().replace_extension(".json")).string();

    // Need to drop _eval filename suffix?
    if (filename.substr(filename.length() - 5 - 4, 5) == "_eval" &&
        !fileExists(jsonFile)) {
        jsonFile = filename.substr(0, filename.length() - 5 - 4) + ".json";
    }

    if (!fileExists(jsonFile)) {
        // Check for a mappings.json file
        std::string mappingJson = p.replace_filename("mappings.json").string();
        if (fileExists(mappingJson)) {
            jsonFile = mappingJson;
        }
    }

    std::ifstream fin(jsonFile);
    std::unordered_map<int, std::string> res;

    if (fin.good()) {
        std::cout << "Reading classification information: " << jsonFile << std::endl;
        json data = json::parse(fin);
        if (data.contains("classification")) {
            auto c = data["classification"];
            for (auto it = c.begin(); it != c.end(); ++it) {
                res[std::stoi(it.key())] = it.value().get<std::string>();
                std::cout << it.key() << ": " << it.value().get<std::string>() << std::endl;
            }
        }
        else {
            std::cout << "Error: Invalid JSON (no mapping will be applied)" << std::endl;
        }
    }

    return res;
}

bool fileExists(const std::string &path) {
    std::ifstream fin(path);
    const bool e = fin.good();
    fin.close();
    return e;
}
