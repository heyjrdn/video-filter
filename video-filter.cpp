#include "Jzon.h"
#include "filters.h"
#include <string>
#include <iostream>
#include <sstream>
#include <vector>
#include <unistd.h>
#include <ctime>

#define SUCCESS 0
#define ERROR   1
#define MT_BEGIN 0
#define MT_END  1
#define SEPIA   0
#define BW      1
#define INVERTED 2

int main_thread_status = MT_BEGIN;
pthread_mutex_t mutex_frame;
int actual_frame = 1;
int total_frames = 0;
int filter_type;
std::string image_sufix;

int checkParameters(Jzon::Object params)
{
    // Search for parameter 'filename'
    try
    {
        params.Get("filename").ToString();
    }
    catch (Jzon::NotFoundException e)
    {
        std::cout << "Parameter 'filename' missing." << std::endl;
        return ERROR;
    }

    // Search for parameter 'filter'
    try
    {
        params.Get("filter").ToString();
    }
    catch (Jzon::NotFoundException e)
    {
        std::cout << "Parameter 'filter' missing." << std::endl;
        return ERROR;
    }

    // Search for parameter 'image-sufix'
    try
    {
        params.Get("images").Get("sufix").ToString();
    }
    catch (Jzon::NotFoundException e)
    {
        std::cout << "Parameter 'threads-video' missing." << std::endl;
        return ERROR;
    }

    // Search for parameter 'thread-video'
    try
    {
        params.Get("threads").Get("video").ToString();
    }
    catch (Jzon::NotFoundException e)
    {
        std::cout << "Parameter 'threads-video' missing." << std::endl;
        return ERROR;
    }

    // Search for parameter 'thread-images'
    try
    {
        params.Get("threads").Get("images").ToString();
    }
    catch (Jzon::NotFoundException e)
    {
        std::cout << "Parameter 'threads-images' missing." << std::endl;
        return ERROR;
    }

    return SUCCESS;
}


std::string exec(const char* cmd)
{
    char buffer[128];
    FILE* pipe;
    std::string result = "";

    pipe = popen(cmd, "r");

    if (!pipe)
    {
        return "ERROR";
    }

    while (!feof(pipe))
    {
        if(fgets(buffer, 128, pipe) != NULL)
        {
            result += buffer;
        }
    }

    pclose(pipe);

    return result;
}


std::vector<std::string> &split(const std::string &s,
                                char delim,
                                std::vector<std::string> &elems)
{
    std::stringstream ss(s);
    std::string item;

    while (std::getline(ss, item, delim))
    {
        elems.push_back(item);
    }

    return elems;
}


std::vector<std::string> split(const std::string &s,
                               char delim)
{
    std::vector<std::string> elems;

    split(s, delim, elems);

    return elems;
}


void checkVideInfo(Jzon::Object params,
                   float *duration,
                   float *frames,
                   std::string *filetype,
                   std::string *image_sufix,
                   int *threads_images)
{
    const char *check_info_cmd;
    std::vector<std::string> info_result, file_result, times;
    std::string::size_type string_size_type;

    check_info_cmd = "python check_info.py";

    info_result = split(exec(check_info_cmd), ',');

    // set duration (in seconds)
    times = split(info_result[0], ':');
    *duration += std::stof(times[0], &string_size_type) * 3600; // hours
    *duration += std::stof(times[1], &string_size_type) * 60; // minutes
    *duration += std::stof(times[2], &string_size_type); // seconds

    // set frames
    *frames = std::stof(info_result[1], &string_size_type);

    // set filetype
    file_result = split(params.Get("filename").ToString(), '.');
    *filetype = file_result[file_result.size() - 1];

    // set image prefix
    *image_sufix = params.Get("images").Get("sufix").ToString();

    // set threads-images count
    *threads_images = params.Get("threads").Get("images").ToInt();
}


void extractFrames(std::string filename, float frames, std::string image_sufix)
{
    std::string extract_frames_cmd;
    std::stringstream ss_frames;

    // Convert float to string
    ss_frames << frames;

    // Build code
    extract_frames_cmd = "ffmpeg -i " + filename +
                         " -r " + ss_frames.str() +
                         " ./resources/tmp/%d." + image_sufix;

    exec(extract_frames_cmd.c_str());
}


void compileVideo(std::string filename, float frames, std::string image_sufix)
{
    std::string extract_frames_cmd;
    std::stringstream ss_frames;

    // Convert float to string
    ss_frames << frames;

    // Build code
    extract_frames_cmd = "ffmpeg -f image2 -r " + ss_frames.str() +
                         " -i \"./resources/tmp/%d." + image_sufix + "\" " +
                         filename;

    exec(extract_frames_cmd.c_str());
}

void *applyFilter(void *threadID)
{
    long tID;
    int this_frame;
    tID = (long)threadID;
    std::string filename;
    std::string s_this_frame;

    while (main_thread_status != MT_END || actual_frame < total_frames)
    {
        std::vector<unsigned char> image;
        unsigned width, height;

        // Lock a mutex prior to updating the value
        pthread_mutex_lock (&mutex_frame);
        this_frame = actual_frame;
        actual_frame = actual_frame + 1;
        pthread_mutex_unlock (&mutex_frame);

        s_this_frame = std::to_string(this_frame);

        filename = "./resources/tmp/" + s_this_frame + "." + image_sufix;

        // verify that the file exists
        while (ERROR ==
               decodeOneStep(filename.c_str(),&image, &width, &height)
               && main_thread_status == MT_BEGIN)
        {
            // wait 0.1 seconds until the file exists
            usleep(100000);
        }

        switch (filter_type)
        {
            case SEPIA:
            {
                sepiaFilter(&image, width, height);
            }
                break;
            case BW:
            {
                grayFilter(&image, width, height);
            }
                break;
            case INVERTED:
            {
                invertedFilter(&image, width, height);
            }
                break;
        }

        encodeOneStep(filename.c_str(), image, width, height);
    }

    pthread_exit(NULL);
}


int main(int argc, char* argv[])
{
    Jzon::Object params;
    float duration = 0.0;
    float frames = 0.0;
    std::string filetype = "";
    std::string images_count;
    pthread_attr_t attr;
    int thread_result;
    int threads_images;
    void *status;
    time_t start,end;


    exec("rm ./resources/output.mp4");

    // Read parameters from the JSON file
    Jzon::FileReader::ReadFile("params.json", params);

    // Check the parameters so the complete info is loaded
    if (ERROR == checkParameters(params))
    {
        return ERROR;
    }

    // Grab the neccesary info from the video file
    checkVideInfo(params,
                  &duration,
                  &frames,
                  &filetype,
                  &image_sufix,
                  &threads_images);

    // set filter type
    filter_type = params.Get("filter").ToInt();

    // Create threads data
    pthread_t threads[threads_images];
    pthread_mutex_init(&mutex_frame, NULL);

    // Initialize and set thread detached attribute
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    time(&start);

    // With p_threads calls the filter method for each image
    for (int i = 0; i < threads_images; i++)
    {
        thread_result = pthread_create(&threads[i],
                                       &attr,
                                       applyFilter,
                                       NULL);
    }

    // Get frames of video
    extractFrames(params.Get("filename").ToString(), frames, image_sufix);

    // set total frames
    images_count = exec("ls -1 ./resources/tmp/ | wc -l");

    total_frames = std::stoi(images_count);

    // Set the end of the frames
    main_thread_status = MT_END;

    /* Free attribute and wait for the other threads */
    pthread_attr_destroy(&attr);
    for (int i = 0; i < threads_images; i++)
    {
        thread_result = pthread_join(threads[i], &status);
    }

    // Recompile the video
    compileVideo("./resources/output." + filetype, frames, image_sufix);

    time(&end);

    printf("Seconds: %lu\n", end-start );

    exec("rm ./resources/tmp/*.png");

    /* Last thing that main() should do */
    pthread_mutex_destroy(&mutex_frame);
    pthread_exit(NULL);
}